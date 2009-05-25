/* <x0/mod_accesslog.cpp>
 *
 * This file is part of the x0 web server, released under GPLv3.
 * (c) 2009 Chrisitan Parpart <trapni@gentoo.org>
 */

#include <x0/server.hpp>
#include <x0/request.hpp>
#include <x0/response.hpp>
#include <x0/header.hpp>
#include <x0/strutils.hpp>
#include <x0/types.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/bind.hpp>
#include <cstring>
#include <cerrno>

/**
 * \ingroup modules
 * \brief implements an accesslog log facility - in spirit of "combined" mode of apache's accesslog logs.
 */
class accesslog_plugin :
	public x0::plugin
{
private:
	boost::signals::connection c;

public:
	accesslog_plugin(x0::server& srv) :
		x0::plugin(srv)
	{
		c = srv.request_done.connect(boost::bind(&accesslog_plugin::request_done, this, _1, _2));
	}

	~accesslog_plugin()
	{
		server_.request_done.disconnect(c);
	}

	virtual void configure()
	{
		// TODO retrieve file to store accesslog log to.
	}

private:
	void request_done(x0::request& in, x0::response& out)
	{
		std::stringstream sstr;
		sstr << hostname(in);
		sstr << " - "; // identity as of identd
		sstr << username(in) << ' ';
		sstr << now() << " \"";
		sstr << request_line(in) << "\" ";
		sstr << out.status << ' ';
		sstr << out.content.length() << ' ';
		sstr << '"' << get_header(in, "Referer") << "\" ";
		sstr << '"' << get_header(in, "User-Agent") << '"';

		std::cout << sstr.str() << std::endl;
	}

	inline std::string hostname(x0::request& in)
	{
		std::string name = in.connection->socket().remote_endpoint().address().to_string();
		return !name.empty() ? name : "-";
	}

	inline std::string username(x0::request& in)
	{
		return !in.username.empty() ? in.username : "-";
	}

	inline std::string request_line(x0::request& in)
	{
		std::stringstream str;

		str << in.method << ' ' << in.uri
			<< " HTTP/" << in.http_version_major << '.' << in.http_version_minor;

		return str.str();
	}

	inline std::string now()
	{
		char buf[26];
		std::time_t ts = time(0);

		if (struct tm *tm = localtime(&ts))
		{
			if (strftime(buf, sizeof(buf), "[%m/%d/%y:%T %z]", tm) != 0)
			{
				return buf;
			}
		}
		return "-";
	}

	inline std::string get_header(const x0::request& in, const std::string& name)
	{
		std::string value(in.get_header(name));
		return !value.empty() ? value : "-";
	}
};

extern "C" void accesslog_init(x0::server& srv) {
	srv.setup_plugin(x0::plugin_ptr(new accesslog_plugin(srv)));
}