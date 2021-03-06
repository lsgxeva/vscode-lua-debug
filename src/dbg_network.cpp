#include "dbg_network.h"

#define NETLOG_BACKEND NETLOG_EMPTY_BACKEND	
#include <iostream>
#include <memory>
#include <string>	
#include <thread> 	
#include <net/poller.h>
#include <net/tcp/connecter.h>
#include <net/tcp/listener.h> 
#include <net/tcp/stream.h>	
#include <rapidjson/schema.h> 
#include <rapidjson/error/en.h>		
#include "dbg_protocol.h" 
#include "dbg_file.h"  		 
#include "dbg_format.h"

#if 1	 
#	define log(...)
#else
template <class... Args>
static void log(const char* fmt, const Args& ... args)
{
	vscode::printf(fmt, args...);
}
#endif	 

namespace vscode
{
	class server;
	class rprotocol;
	class wprotocol;

	class shortconnect
		: public net::tcp::connecter 
	{
	public:
		typedef net::tcp::connecter base_type;
		shortconnect(net::poller_t* poll, const char* ip, uint16_t port)
		: base_type(poll)
		{
			base_type::connect(net::endpoint(ip, port));
		}

		bool try_close() {
			if (write_empty()) {
				close();
				return true;
			}
			return false;
		}
	};

	class session
		: public net::tcp::stream
	{
	public:
		typedef net::tcp::stream base_type;

	public:
		session(server* server, net::poller_t* poll);
		bool      output(const wprotocol& wp);
		rprotocol input();
		bool      input_empty() const;
		bool      open_schema(const std::string& schema_file);

	private:
		void event_close();
		bool event_in();
		bool event_message(const std::string& buf, size_t start, size_t count);
		bool unpack();

	private:
		server* server_;
		std::string stream_buf_;
		size_t      stream_stat_;
		size_t      stream_len_;
		net::tcp::buffer<rprotocol, 8> input_queue_;
		std::unique_ptr<rapidjson::SchemaDocument> schema_;
	};

	class server
		: public net::tcp::listener
	{
	public:
		friend class session;
		typedef net::tcp::listener base_type;

	public:
		server(net::poller_t* poll, const net::endpoint& ep, bool rebind);
		~server();
		void      update();
		bool      listen();
		bool      output(const wprotocol& wp);
		rprotocol input();
		bool      input_empty()	const;
		void      set_schema(const char* file);
		void      close_session(); 
		uint16_t  get_port() const;

	private:
		void event_accept(net::socket::fd_t fd, const net::endpoint& ep);
		void event_close();

	private:
		std::unique_ptr<session> session_;
		std::string              schema_file_;
		net::endpoint            endpoint_;
		std::vector<std::unique_ptr<session>> clearlist_;
		bool                     rebind_;
		std::unique_ptr<shortconnect> shortconnect_;
	};

	session::session(server* server, net::poller_t* poll)
		: net::tcp::stream(poll)
		, server_(server)
		, stream_stat_(0)
		, stream_buf_()
		, stream_len_(0)
	{
	}

	bool session::input_empty() const
	{
		return input_queue_.empty();
	}

	bool session::output(const wprotocol& wp)
	{
		if (!wp.IsComplete())
			return false;
		log("%s\n", wp.data());
		send(format("Content-Length: %d\r\n\r\n", wp.size()));
		send(wp);
		return true;
	}

	rprotocol session::input()
	{
		rprotocol r = std::move(input_queue_.front());
		input_queue_.pop();
		return r;
	}

	void session::event_close()
	{
		base_type::event_close();
		server_->close_session();
	}

	bool session::event_in()
	{
		if (!base_type::event_in())
			return false;
		if (!unpack())
			return false;
		return true;
	}

	bool session::unpack()
	{
		for (size_t n = base_type::recv_size(); n; --n)
		{
			char c = 0;
			base_type::recv(&c, 1);
			stream_buf_.push_back(c);
			switch (stream_stat_)
			{
			case 0:
				if (c == '\r') stream_stat_ = 1;
				break;
			case 1:
				stream_stat_ = 0;
				if (c == '\n')
				{
					if (stream_buf_.substr(0, 16) != "Content-Length: ")
					{
						return false;
					}
					try {
						stream_len_ = (size_t)std::stol(stream_buf_.substr(16, stream_buf_.size() - 18));
						stream_stat_ = 2;
					}
					catch (...) {
						return false;
					}
					stream_buf_.clear();
				}
				break;
			case 2:
				if (stream_buf_.size() >= (stream_len_ + 2))
				{
					if (!event_message(stream_buf_, 2, stream_len_))
						return false;
					stream_buf_.clear();
					stream_stat_ = 0;
				}
				break;
			}
		}
		return true;
	}

	bool session::event_message(const std::string& buf, size_t start, size_t count)
	{
		rapidjson::Document	d;
		if (d.Parse(buf.data() + start, count).HasParseError())
		{
			log("Input is not a valid JSON\n");
			log("Error(offset %u): %s\n", static_cast<unsigned>(d.GetErrorOffset()), rapidjson::GetParseError_En(d.GetParseError()));
			return true;
		}
		if (schema_)
		{
			rapidjson::SchemaValidator validator(*schema_);
			if (!d.Accept(validator))
			{
				rapidjson::StringBuffer sb;
				validator.GetInvalidSchemaPointer().StringifyUriFragment(sb);
				log("Invalid schema: %s\n", sb.GetString());
				log("Invalid keyword: %s\n", validator.GetInvalidSchemaKeyword());
				sb.Clear();
				validator.GetInvalidDocumentPointer().StringifyUriFragment(sb);
				log("Invalid document: %s\n", sb.GetString());
				return true;
			}
		}
		log("%s\n", buf.substr(start, count).c_str());
		input_queue_.push(rprotocol(std::move(d)));
		return true;
	}

	bool session::open_schema(const std::string& schema_file)
	{
		file file(schema_file.c_str(), std::ios_base::in);
		if (!file.is_open())
		{
			return false;
		}
		std::string buf = file.read<std::string>();
		rapidjson::Document sd;
		if (sd.Parse(buf.data(), buf.size()).HasParseError())
		{
			log("Input is not a valid JSON\n");
			log("Error(offset %u): %s\n", static_cast<unsigned>(sd.GetErrorOffset()), rapidjson::GetParseError_En(sd.GetParseError()));
			return false;
		}
		schema_.reset(new rapidjson::SchemaDocument(sd));
		return true;
	}

	server::server(net::poller_t* poll, const net::endpoint& ep, bool rebind)
		: base_type(poll)
		, session_()
		, endpoint_(ep)
		, rebind_(false)
	{
		net::socket::initialize();
		base_type::open();
	}

	server::~server()
	{
		base_type::close();
		if (session_)
			session_->close();
	}

	void server::update()
	{
		if (!is_listening()) {
			listen();
		}
		for (auto& s = clearlist_.begin(); s != clearlist_.end();)
		{
			if ((*s)->sock == net::socket::retired_fd) {
				s = clearlist_.erase(s);
			}
			else {
				++s;
			}
		}
		if (shortconnect_ && shortconnect_->try_close()) {
			shortconnect_.reset();
		}
	}

	bool server::listen()
	{
		bool ok = base_type::listen(endpoint_, rebind_, std::bind(&server::event_accept, this, std::placeholders::_1, std::placeholders::_2));
		if (ok) {
			const wchar_t* env = _wgetenv(L"LUADBG_PORT");
			if (env) {
				int port = _wtoi(env);
				if (port) {
					shortconnect_.reset(new shortconnect(poller_, "127.0.0.1", port));
					shortconnect_->send(std::to_string(get_port()));
				}
			}
		}
		return ok;
	}

	void server::event_accept(net::socket::fd_t fd, const net::endpoint& ep)
	{
		if (session_)
		{
			net::socket::close(fd);
			return;
		}
		session_.reset(new session(this, get_poller()));
		if (!schema_file_.empty())
		{
			session_->open_schema(schema_file_);
		}
		session_->attach(fd, ep);
	}

	void server::event_close()
	{
		session_.reset();
		base_type::event_close();
	}

	bool server::output(const wprotocol& wp)
	{
		if (!session_)
			return false;
		return session_->output(wp);
	}

	rprotocol server::input()
	{
		if (!session_)
			return rprotocol();
		if (session_->input_empty())
			return rprotocol();
		return session_->input();
	}

	bool server::input_empty()	const
	{
		if (!session_)
			return false;
		return session_->input_empty();
	}

	void server::set_schema(const char* file)
	{
		schema_file_ = file;
	}

	void server::close_session()
	{
		if (!session_)
			return;
		std::unique_ptr<session> s(session_.release());
		s->close();
		clearlist_.push_back(std::move(s));
	}

	uint16_t server::get_port() const
	{
		if (sock == net::socket::retired_fd) {
			return 0;
		}
		sockaddr_in addr;
		int addrlen = sizeof(sockaddr_in);
		::getsockname(sock, (sockaddr*)&addr, &addrlen);
		return ::ntohs(addr.sin_port);
	}

	network::network(const char* ip, uint16_t port, bool rebind)
		: poller_(new net::poller_t)
		, server_(new server(poller_, net::endpoint(ip, port), rebind))
		, kill_process_when_close_(false)
	{
		server_->listen();
	}

	network::~network()
	{
		delete server_;
		delete poller_;
	}

	void network::update(int ms)
	{
		server_->update();
		poller_->wait(1000, ms);
	}

	bool network::output(const wprotocol& wp)
	{
		if (!server_)
			return false;
		if (!server_->output(wp))
			return false;
		return true;
	}

	rprotocol network::input()
	{
		if (!server_)
			return rprotocol();
		return server_->input();
	}

	bool network::input_empty() const
	{
		if (!server_)
			return false;
		return server_->input_empty();
	}

	void network::set_schema(const char* file)
	{
		if (!server_)
			return;
		return server_->set_schema(file);
	}

	void network::close()
	{
		if (server_)
			server_->close_session();
		if (kill_process_when_close_)
			exit(0);
	}

	void network::kill_process_when_close()
	{
		kill_process_when_close_ = true;
	}

	uint16_t network::get_port() const
	{
		if (server_)
			return server_->get_port();
		return 0;
	}
}
