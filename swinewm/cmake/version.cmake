# The version comes from the git tag. On a tagged commit ("0.1.0" or
# "v0.1.0") it is that tag; later commits are "0.1.0-5-g1234abc" (five
# commits after 0.1.0), with "-dirty" for uncommitted changes. Without git
# (a source archive) SWINEWM_FALLBACK_VERSION below is used.
#
# Sets SWINEWM_VERSION_NUMERIC (X.Y.Z.N, N = commits since the tag; for
# project(), the .exe version resource and the MSI, which upgrades any
# build with the same or a lower X.Y.Z) and SWINEWM_VERSION (the
# human-readable one, shown by "swinewm -v" and in file names).

set(SWINEWM_FALLBACK_VERSION 0.0.1)

set(SWINEWM_VERSION_NUMERIC "${SWINEWM_FALLBACK_VERSION}.0")
set(SWINEWM_VERSION "${SWINEWM_FALLBACK_VERSION}-unknown")

find_program(SWINEWM_GIT git)
if(SWINEWM_GIT)
  execute_process(
    COMMAND "${SWINEWM_GIT}" describe --tags --long --dirty
      --match "[0-9]*.[0-9]*.[0-9]*" --match "v[0-9]*.[0-9]*.[0-9]*"
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
    OUTPUT_VARIABLE _describe
    RESULT_VARIABLE _result
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(_result EQUAL 0 AND _describe MATCHES
      "^v?([0-9]+)\\.([0-9]+)\\.([0-9]+)-([0-9]+)-g([0-9a-f]+)(-dirty)?$")
    set(_xyz "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
    set(_commits "${CMAKE_MATCH_4}")
    set(SWINEWM_VERSION_NUMERIC "${_xyz}.${_commits}")
    if(_commits EQUAL 0 AND NOT CMAKE_MATCH_6)
      set(SWINEWM_VERSION "${_xyz}")
    else()
      set(SWINEWM_VERSION
        "${_xyz}-${_commits}-g${CMAKE_MATCH_5}${CMAKE_MATCH_6}")
    endif()
  else()
    message(STATUS "No version tag found in git; using the fallback version")
  endif()

  # Configure again after commits and checkouts (and tags that git packed),
  # so the version does not go stale in an existing build directory. Only
  # files: MSBuild rejects directories as dependencies. Tagging the current
  # commit without a new commit needs "cmake ." to show the new version.
  execute_process(
    COMMAND "${SWINEWM_GIT}" rev-parse --absolute-git-dir
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
    OUTPUT_VARIABLE _git_dir
    RESULT_VARIABLE _result
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(_result EQUAL 0)
    foreach(_file logs/HEAD packed-refs)
      if(EXISTS "${_git_dir}/${_file}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
          "${_git_dir}/${_file}")
      endif()
    endforeach()
  endif()
endif()
message(STATUS "SwineWM version ${SWINEWM_VERSION}")
