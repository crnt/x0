add_definitions(
	-Dlinux=1
	-pthread
	-std=c++0x
	-DPACKAGE_NAME="${PACKAGE_NAME}"
	-DPACKAGE_VERSION="${PACKAGE_VERSION}"
	-DPACKAGE_HOMEPAGE_URL="${PACKAGE_HOMEPAGE_URL}"
	-DSYSCONFDIR="${SYSCONFDIR}"
	-DPLUGINDIR="${PLUGINDIR}"
	-DVERSION="${PACKAGE_VERSION}")

add_executable(x0d x0d.cpp)
target_link_libraries(x0d XzeroBase XzeroHttp)

install(TARGETS x0d RUNTIME DESTINATION sbin)
install(FILES x0d.conf-dist DESTINATION etc)
