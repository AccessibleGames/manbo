#include <gtest/gtest.h>

#include "repositories/sample_repository.h"

namespace manbo {
namespace {
TEST(Repository, ExactSymbolRanksFirst) {
  SampleRepository repository;
  auto hits = repository.Search("std::forward_list", {});
  ASSERT_TRUE(hits);
  ASSERT_GE(hits->size(), 2u);
  EXPECT_EQ(hits->front().id, "cpp/forward-list");
}

TEST(Repository, KeywordsAndFuzzyNamesWorkOffline) {
  SampleRepository repository;
  auto keywords = repository.Search("singly linked", {});
  ASSERT_TRUE(keywords);
  EXPECT_FALSE(keywords->empty());
  auto fuzzy = repository.Search("forward lst", {});
  ASSERT_TRUE(fuzzy);
  ASSERT_FALSE(fuzzy->empty());
  EXPECT_EQ(fuzzy->front().id, "cpp/forward-list");
}

TEST(Repository, SectionsAreStringsAndNamesMustBeDisambiguated) {
  SampleRepository repository;
  auto ambiguous = repository.Read("printf", {});
  ASSERT_FALSE(ambiguous);
  EXPECT_EQ(ambiguous.error().code, ErrorCode::kAmbiguous);
  auto page = repository.Read("printf(3)", {});
  ASSERT_TRUE(page);
  EXPECT_EQ(page->manual_section, "3");
  EXPECT_EQ(page->id, "man/printf/3");
  auto hits = repository.Search("PRINTF(3)", {});
  ASSERT_TRUE(hits);
  ASSERT_FALSE(hits->empty());
  EXPECT_EQ(hits->front().manual_section, "3");
}

TEST(Repository, EmptyAndMissingQueriesAreSuccessfulValues) {
  SampleRepository repository;
  auto all = repository.Search("", {});
  ASSERT_TRUE(all);
  EXPECT_EQ(all->size(), 6u);
  auto none = repository.Search("this-page-does-not-exist-987", {});
  ASSERT_TRUE(none);
  EXPECT_TRUE(none->empty());
  auto missing = repository.Read("this-page-does-not-exist-987", {});
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().code, ErrorCode::kNotFound);
}

TEST(Repository, CancellationIsAnExplicitExpectedError) {
  SampleRepository repository;
  std::stop_source stop;
  stop.request_stop();
  auto search = repository.Search("vector", stop.get_token());
  ASSERT_FALSE(search);
  EXPECT_EQ(search.error().code, ErrorCode::kCancelled);
  auto read = repository.Read("cpp/vector", stop.get_token());
  ASSERT_FALSE(read);
  EXPECT_EQ(read.error().code, ErrorCode::kCancelled);
}

}  // namespace
}  // namespace manbo
