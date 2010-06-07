#ifndef sw_x0_HttpCore_h
#define sw_x0_HttpCore_h 1

#include <x0/http/HttpPlugin.h>

namespace x0 {

class HttpServer;
class SettingsValue;
class Scope;

class HttpCore :
	public HttpPlugin
{
public:
	explicit HttpCore(HttpServer& server);
	~HttpCore();

	property<unsigned long long> max_fds;

private:
	long long getrlimit(int resource);
	long long setrlimit(int resource, long long max);

	bool setup_logging(const SettingsValue& cvar, Scope& s);
	bool setup_resources(const SettingsValue& cvar, Scope& s);
	bool setup_modules(const SettingsValue& cvar, Scope& s);
	bool setup_fileinfo(const SettingsValue& cvar, Scope& s);
	bool setup_error_documents(const SettingsValue& cvar, Scope& s);
	bool setup_hosts(const SettingsValue& cvar, Scope& s);
	bool setup_advertise(const SettingsValue& cvar, Scope& s);
};

} // namespace x0

#endif