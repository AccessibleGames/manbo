# Each source-family adapter owns a target and a subdirectory. The dispatcher
# in documents/content.cc depends on them through manbo_content.
add_subdirectory(html)
add_subdirectory(plain)
add_subdirectory(cppreference)
add_subdirectory(posix)
add_subdirectory(gnu)
add_subdirectory(linux_man)
