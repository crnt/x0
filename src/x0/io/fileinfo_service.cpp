#include <x0/io/fileinfo_service.hpp>
#include <x0/strutils.hpp>

#include <boost/tokenizer.hpp>

namespace x0 {

fileinfo_service::fileinfo_service(struct ::ev_loop *loop) :
	loop_(loop),
#if defined(HAVE_SYS_INOTIFY_H)
	handle_(),
	inotify_(loop_),
	wd_(),
#endif
	cache_(),
	etag_consider_mtime_(true),
	etag_consider_size_(true),
	etag_consider_inode_(false),
	mimetypes_(),
	default_mimetype_("text/plain")
{
#if defined(HAVE_SYS_INOTIFY_H)
	handle_ = inotify_init();
	if (handle_ != -1)
	{
		fcntl(handle_, F_SETFL, O_NONBLOCK | FD_CLOEXEC);

		inotify_.set<fileinfo_service, &fileinfo_service::on_inotify>(this);
		inotify_.start(handle_, ev::READ);
	}
#endif
}

fileinfo_service::~fileinfo_service()
{
#if defined(HAVE_SYS_INOTIFY_H)
	if (handle_ != -1)
	{
		::close(handle_);
	}
#endif
}

void fileinfo_service::on_inotify(ev::io& w, int revents)
{
	DEBUG("fileinfo_service::on_inotify()");

	char buf[4096];
	ssize_t rv = ::read(handle_, &buf, sizeof(buf));
	if (rv > 0)
	{
		inotify_event *i = (inotify_event *)buf;
		inotify_event *e = i + rv;

		while (i < e && i->wd != 0)
		{
			auto wi = wd_.find(i->wd);
			if (wi != wd_.end())
			{
				auto k = cache_.find(wi->second);
				// on_invalidate(k->first, k->second);
				cache_.erase(k);
				wd_.erase(wi);
			}
			i += sizeof(*i) + i->len;
		}
	}
}

void fileinfo_service::load_mimetypes(const std::string& filename)
{
	typedef boost::tokenizer<boost::char_separator<char> > tokenizer;

	std::string input(x0::read_file(filename));
	tokenizer lines(input, boost::char_separator<char>("\n"));

	mimetypes_.clear();

	for (tokenizer::iterator i = lines.begin(), e = lines.end(); i != e; ++i)
	{
		std::string line(x0::trim(*i));
		tokenizer columns(line, boost::char_separator<char>(" \t"));

		tokenizer::iterator ci = columns.begin(), ce = columns.end();
		std::string mime = ci != ce ? *ci++ : std::string();

		if (!mime.empty() && mime[0] != '#')
		{
			for (; ci != ce; ++ci)
			{
				mimetypes_[*ci] = mime;
			}
		}
	}
}

} // namespace x0
