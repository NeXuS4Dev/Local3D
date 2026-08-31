l3d_add_module(
  NAME Core
  NAMESPACE Core
  SOURCES src/Assert.cpp
          src/Log.cpp
          src/Profiler.cpp
          src/Status.cpp
          src/Time.cpp
          src/Uuid.cpp
          src/jobs/JobSystem.cpp
          src/c/l3d_log_c.cpp
          src/c/l3d_allocator_c.cpp
  HEADERS include/local3d/core/Assert.hpp
          include/local3d/core/Common.hpp
          include/local3d/core/ConcurrentQueue.hpp
          include/local3d/core/Enum.hpp
          include/local3d/core/Format.hpp
          include/local3d/core/Hash.hpp
          include/local3d/core/Log.hpp
          include/local3d/core/Profiler.hpp
          include/local3d/core/Result.hpp
          include/local3d/core/Signal.hpp
          include/local3d/core/StaticString.hpp
          include/local3d/core/Status.hpp
          include/local3d/core/ThreadId.hpp
          include/local3d/core/Time.hpp
          include/local3d/core/Uuid.hpp
          include/local3d/core/c/l3d_allocator.h
          include/local3d/core/c/l3d_log.h
          include/local3d/core/jobs/JobSystem.hpp)

target_link_libraries(Local3D_Core PUBLIC Threads::Threads)
