#include <x0/io/connection_sink.hpp>
#include <x0/connection.hpp>
#include <x0/io/source.hpp>
#include <x0/io/file_source.hpp>
#include <x0/io/buffer_source.hpp>
#include <x0/io/filter_source.hpp>
#include <x0/io/composite_source.hpp>

#include <sys/sendfile.h>

namespace x0 {

connection_sink::connection_sink(x0::connection *conn) :
	fd_sink(conn->handle()),
	connection_(conn),
	rv_(0)
{
}

ssize_t connection_sink::pump(source& src)
{
	// call pump-handler
	src.accept(*this);

	return rv_;
}

void connection_sink::visit(fd_source& v)
{
	rv_ = fd_sink::pump(v);
}

void connection_sink::visit(file_source& v)
{
	if (!offset_)
		offset_ = v.offset(); // initialize with the starting-offset of interest

	if (std::size_t remaining = v.count() - offset_) // how many bytes are still to process
		rv_ = sendfile(handle(), v.handle(), &offset_, remaining);
	else
		rv_ = 0;
}

void connection_sink::visit(buffer_source& v)
{
	rv_ = fd_sink::pump(v);
}

void connection_sink::visit(filter_source& v)
{
	rv_ = fd_sink::pump(v);
}

void connection_sink::visit(composite_source& v)
{
	rv_ = fd_sink::pump(v);
}

} // namespace x0