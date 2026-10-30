#ifndef MANBO_SAMPLE_REPOSITORY_H_
#define MANBO_SAMPLE_REPOSITORY_H_

#include "core/interfaces.h"

namespace manbo {

// Self-authored fixture corpus, not a real man/HTML importer.
class SampleRepository final : public IDocumentRepository {
 public:
  SampleRepository();
  Result<std::vector<SearchHit>>
  Search(std::string_view query, std::stop_token stop) override;
  Result<Document>
  Read(std::string_view page_id, std::stop_token stop) override;

 private:
  std::vector<Document> documents_;
};

}  // namespace manbo

#endif  // MANBO_SAMPLE_REPOSITORY_H_
