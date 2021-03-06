/* <Filter.h>
 *
 * This file is part of the x0 web server project and is released under LGPL-3.
 * http://www.xzero.ws/
 *
 * (c) 2009-2010 Christian Parpart <trapni@gentoo.org>
 */

#ifndef sw_x0_io_filter_hpp
#define sw_x0_io_filter_hpp 1

#include <x0/Buffer.h>
#include <x0/io/Source.h>
#include <x0/io/Sink.h>
#include <x0/Api.h>
#include <memory>

namespace x0 {

//! \addtogroup io
//@{

/** unidirectional data processor.
 *
 * a filter is a processor, that reads from a source, and passes 
 * the received data to the sink. this data may or may not be
 * transformed befor passing to the sink.
 *
 * \see source, sink
 */
class X0_API Filter
{
public:
	/** processes given input data through this Filter. */
	virtual Buffer process(const BufferRef& input) = 0;

public:
	Buffer operator()(const BufferRef& input)
	{
		return process(input);
	}
};

typedef std::shared_ptr<Filter> FilterPtr;

//@}

} // namespace x0

#endif
