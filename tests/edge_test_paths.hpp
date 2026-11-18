#ifndef EDGE_TEST_PATHS_HPP
#define EDGE_TEST_PATHS_HPP

// Source-tree locations the unit tests read their fixtures from.
//
// tests/CMakeLists.txt force-includes this header into every test translation unit (test_common), so
// TEST_DATA_DIR and friends keep working without an #include. The paths themselves are compiled into
// ONE generated translation unit, <build>/tests/edge_test_paths.cpp (target test_data_paths), instead of
// being passed as -D values to every test source. That keeps the compile command and the preprocessed
// output of the test sources identical in every worktree, so a compiler cache (sccache on the build VM)
// can reuse their objects across worktrees; only the generated file is specific to one tree.
//
// The macros expand to expressions of type const char *, not to string literals: write
// std::string(TEST_DATA_DIR) + "/x" or boost::filesystem::path(TEST_DATA_DIR) / "x", never
// TEST_DATA_DIR "/x" (literal concatenation).

namespace edge_test {
// <source>/tests/data, native separators (backslashes on Windows), as the old -DTEST_DATA_DIR was.
const char *test_data_dir();
// <source>/resources, forward slashes (the old -DSLIC3R_TEST_RESOURCES_DIR of slic3rutils_tests).
const char *resources_dir();
// <source>/tests/slic3rutils/slice_v1 and .../paint_v1, forward slashes (slic3rutils_tests fixtures).
const char *slice_v1_fixtures_dir();
const char *paint_v1_fixtures_dir();
} // namespace edge_test

#define TEST_DATA_DIR (::edge_test::test_data_dir())
#define SLIC3R_TEST_RESOURCES_DIR (::edge_test::resources_dir())
#define SLICE_V1_FIXTURES_DIR (::edge_test::slice_v1_fixtures_dir())
#define PAINT_V1_FIXTURES_DIR (::edge_test::paint_v1_fixtures_dir())

#endif // EDGE_TEST_PATHS_HPP
