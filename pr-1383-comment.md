Hi @wangdianjin!

I reviewed your PR and wanted to share an **alternative implementation** that eliminates the need for git submodules entirely. This approach uses CMake's `FetchContent` module, which I've seen work well in other Apache projects like MADlib.

## Alternative: CMake FetchContent

I've implemented this on branch [`fetchcontent-yyjson`](https://github.com/edespino/cloudberry/tree/fetchcontent-yyjson) for your consideration.

**Key change:** Replace the git submodule with automatic dependency fetching during CMake configuration.

### Implementation

**`contrib/pax_storage/CMakeLists.txt`**:
```cmake
if(USE_MANIFEST_API AND NOT USE_PAX_CATALOG)
    include(FetchContent)

    # Try system package first
    find_package(yyjson QUIET)

    if(NOT yyjson_FOUND)
        message(STATUS "yyjson not found in system, fetching from GitHub...")
        FetchContent_Declare(
            yyjson
            GIT_REPOSITORY https://github.com/ibireme/yyjson.git
            GIT_TAG 0.12.0
            GIT_SHALLOW TRUE
        )

        set(SAVED_BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS})
        set(BUILD_SHARED_LIBS ON)

        FetchContent_MakeAvailable(yyjson)

        set(BUILD_SHARED_LIBS ${SAVED_BUILD_SHARED_LIBS})
    else()
        message(STATUS "Using system yyjson package")
    endif()
endif()
```

**`contrib/pax_storage/src/cpp/cmake/pax.cmake`**:
```cmake
# Update include path to use FetchContent variable
set(pax_target_include ${pax_target_include} ${yyjson_SOURCE_DIR}/src)
```

**Remove from `.gitmodules`** - No submodule entry needed.

### Benefits

1. **Simpler workflow** - No `git submodule update --init --recursive` required
2. **Automatic handling** - CMake downloads yyjson when needed during configuration
3. **Version explicit** - `GIT_TAG 0.12.0` (latest release) is clearer than a commit hash
4. **System package support** - Respects system-installed yyjson if available
5. **Pattern consistency** - Aligns with how protobuf and zstd are already handled via `find_package()`

### Important Note

During my review, I noticed that **yyjson is only used when both `USE_MANIFEST_API=ON` and `USE_PAX_CATALOG=OFF`**, which is not the default configuration. This means:
- Default builds (`./configure --enable-pax`) don't use yyjson at all
- Your reorganization and this alternative have zero impact on normal builds
- The dependency is only relevant for the JSON manifest implementation mode

### Testing

To verify this works when yyjson IS needed:

```bash
cd contrib/pax_storage/build
cmake .. -DUSE_MANIFEST_API=ON -DUSE_PAX_CATALOG=OFF
# Should see: "yyjson not found in system, fetching from GitHub..."
make
```

### Recommendation

Your PR's organizational improvement is valuable regardless. This FetchContent approach is simply an alternative that:
- Reduces the dependency management burden (no submodule commands)
- Follows patterns used in other Apache projects
- Makes the build more self-contained

Both approaches work - this is just offered as food for thought. Happy to discuss!

**Branch for reference:** [`fetchcontent-yyjson`](https://github.com/edespino/cloudberry/tree/fetchcontent-yyjson)
