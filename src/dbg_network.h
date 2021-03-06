#pragma once

#include "dbg_io.h"

#if defined(DEBUGGER_INLINE)
#	define DEBUGGER_API
#else
#	if defined(DEBUGGER_EXPORTS)
#		define DEBUGGER_API __declspec(dllexport)
#	else
#		define DEBUGGER_API __declspec(dllimport)
#	endif
#endif

namespace net {
	struct poller_t;
}

namespace vscode
{
	class server;

	class DEBUGGER_API network
		: public io
	{
	public:
		network(const char* ip, uint16_t port, bool rebind);
		virtual   ~network();
		void      update(int ms);
		bool      output(const wprotocol& wp);
		rprotocol input();
		bool      input_empty() const;
		void      close();
		void      set_schema(const char* file); 
		void      kill_process_when_close();
		uint16_t  get_port() const;

	private:
		net::poller_t* poller_;
		server*        server_;
		bool           kill_process_when_close_;
	};
}
