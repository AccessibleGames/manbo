#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>

#include "repositories/manual_repository.h"
#include "repositories/sample_repository.h"

namespace manbo {
namespace {
TEST(Manual, ActualBundledManualHasUnchangedSourceAndWorksWithoutExternalData) {
  auto repository = WithManboManual();
  auto manual = repository->Read("manbo(1)", {});
  ASSERT_TRUE(manual);
  std::ifstream file(
    std::filesystem::path(MANBO_TEST_SOURCE) / "docs/manbo.1.txt",
    std::ios::binary);
  const std::string original{
    std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  ASSERT_FALSE(original.empty());
  EXPECT_EQ(manual->body, original);
  EXPECT_EQ(manual->manual_section, "1");
  ASSERT_TRUE(manual->content);
  for (const auto* text : {"SYNOPSIS",
         "TERMINAL INPUT",
         "IN-PAGE SEARCH",
         "COMMANDS",
         ":man 3 printf",
         "EXIT STATUS"})
    EXPECT_NE(manual->body.find(text), std::string::npos);
  EXPECT_EQ(repository->Search("", {})->size(), 1u);
  EXPECT_FALSE(repository->Read("printf(3)", {}));
  auto combined = WithManboManual(std::make_unique<SampleRepository>());
  EXPECT_EQ(combined->Search("", {})->size(), 7u);
  EXPECT_EQ(combined->Read("printf", {}).error().code, ErrorCode::kAmbiguous);
  std::stop_source stop;
  stop.request_stop();
  EXPECT_EQ(repository->Read("manbo", stop.get_token()).error().code,
    ErrorCode::kCancelled);
}

}  // namespace
}  // namespace manbo
