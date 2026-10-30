#include "repository_setup.h"

#include <gtest/gtest.h>

namespace manbo {
namespace {
TEST(ApplicationRepository, ConstructionAndBuiltinManualDoNotOpenTheCatalog) {
  RepositoryOptions options;
  options.directory = "an-explicitly-missing-offline-catalog";
  auto repository = OpenAppRepository(options);
  ASSERT_TRUE(repository);
  auto manual = (*repository)->Read("manbo(1)", {});
  ASSERT_TRUE(manual);
  EXPECT_EQ(manual->id, "builtin/manbo/1");
  auto search = (*repository)->Search("printf", {});
  ASSERT_FALSE(search);
  EXPECT_EQ(search.error().code, ErrorCode::kNotFound);
  EXPECT_TRUE((*repository)->Read("manbo(1)", {}));
}

TEST(
  ApplicationRepository, ConflictingExplicitSourcesFailInsteadOfBeingIgnored) {
  RepositoryOptions options;
  options.samples = true;
  options.directory = "unused";
  auto repository = OpenAppRepository(options);
  ASSERT_FALSE(repository);
  EXPECT_EQ(repository.error().code, ErrorCode::kInvalidArgument);
}
}  // namespace
}  // namespace manbo
