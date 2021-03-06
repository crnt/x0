/* <x0/plugins/expire.cpp>
 *
 * This file is part of the x0 web server project and is released under LGPL-3.
 * http://www.xzero.ws/
 *
 * (c) 2009-2011 Christian Parpart <trapni@gentoo.org>
 *
 * --------------------------------------------------------------------------
 *
 * plugin type:
 *     metadata
 *
 * description:
 *     Adds Expires and Cache-Control headers to the response.
 *
 * setup API:
 *     none
 *
 * request processing API:
 *     void expires(absolute_time_or_timespan_from_now);
 *
 * examples:
 *     handler main {
 *         docroot '/srv/www'
 *
 *         if phys.exists
 *             expire phys.mtime + 30 days
 *         else
 *             expire sys.now + 30 secs
 *
 *         staticfile
 *     }
 *
 *     handler main {
 *         docroot '/srv/www'
 *         expire 30 days if phys.exists and not phys.path =$ '.csp'
 *         staticfile
 *     }
 */

#include <x0/http/HttpPlugin.h>
#include <x0/http/HttpServer.h>
#include <x0/http/HttpRequest.h>
#include <x0/http/HttpHeader.h>
#include <x0/DateTime.h>
#include <x0/TimeSpan.h>
#include <x0/strutils.h>
#include <x0/Types.h>

#define TRACE(msg...) this->debug(msg)

/**
 * \ingroup plugins
 * \brief adds Expires and Cache-Control response header
 */
class ExpirePlugin :
	public x0::HttpPlugin
{
public:
	ExpirePlugin(x0::HttpServer& srv, const std::string& name) :
		x0::HttpPlugin(srv, name)
	{
		registerFunction<ExpirePlugin, &ExpirePlugin::expire>("expire", x0::FlowValue::VOID);
	}

private:
	// void expire(datetime / timespan)
	void expire(x0::HttpRequest *r, const x0::FlowParams& args, x0::FlowValue& result)
	{
		if (args.size() < 1)
			return;

		time_t now = r->connection.worker().now().unixtime();
		time_t mtime = r->fileinfo ? r->fileinfo->mtime() : now;
		time_t value = args[0].toNumber();

		// passed a timespan
		if (value < mtime)
			value = value + now;

		// (mtime+span) points to past?
		if (value < now)
			value = now;

		r->responseHeaders.overwrite("Expires", x0::DateTime(value).http_str().str());

		x0::Buffer cc(20);
		cc << "max-age=" << (value - now);

		r->responseHeaders.overwrite("Cache-Control", cc.str());
	}
};

X0_EXPORT_PLUGIN_CLASS(ExpirePlugin)
