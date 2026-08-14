<<<<<<< HEAD
add_library(smhasher STATIC MurmurHash3.cpp include/smhasher/MurmurHash3.h)
target_include_directories(smhasher SYSTEM PUBLIC include)
target_include_directories(smhasher PRIVATE include/smhasher)
=======
add_library(smhasher STATIC MurmurHash3.cpp)
target_include_directories(smhasher INTERFACE .)
>>>>>>> origin/main
