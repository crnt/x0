#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sstream>
#include <ctime>

namespace x0 {

inline std::string fileinfo::filename() const
{
	return filename_;
}

inline bool fileinfo::exists() const
{
	return exists_;
}

inline std::size_t fileinfo::size() const
{
	return stat_.st_size;
}

inline time_t fileinfo::mtime() const
{
	return stat_.st_mtime;
}

inline bool fileinfo::is_directory() const
{
	return S_ISDIR(stat_.st_mode);
}

inline bool fileinfo::is_regular() const
{
	return S_ISREG(stat_.st_mode);
}

inline bool fileinfo::is_executable() const
{
	return stat_.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH);
}

inline const ev_statdata *fileinfo::operator->() const
{
	return &stat_;
}

inline std::string fileinfo::etag() const
{
	return etag_;
}

inline std::string fileinfo::last_modified() const
{
	if (mtime_.empty())
	{
		if (struct tm *tm = std::gmtime(&stat_.st_mtime))
		{
			char buf[256];

			if (std::strftime(buf, sizeof(buf), "%a, %d %b %Y %T GMT", tm) != 0)
			{
				mtime_ = buf;
			}
		}
	}

	return mtime_;
}

inline std::string fileinfo::mimetype() const
{
	return mimetype_;
}

} // namespace x0

// vim:syntax=cpp
