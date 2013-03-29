// Copyright (c) 2013, Cloudera, inc.

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <tr1/memory>

#include "cfile/bloomfile.h"
#include "cfile/cfile.h"
#include "tablet/layer.h"
#include "tablet/cfile_set.h"


DEFINE_bool(consult_bloom_filters, true, "Whether to consult bloom filters on row presence checks");

namespace kudu { namespace tablet {

using cfile::ReaderOptions;
using std::tr1::shared_ptr;

////////////////////////////////////////////////////////////
// Utilities
////////////////////////////////////////////////////////////

static Status OpenReader(Env *env, string dir, size_t col_idx,
                         gscoped_ptr<CFileReader> *new_reader) {
  string path = Layer::GetColumnPath(dir, col_idx);

  // TODO: somehow pass reader options in schema
  ReaderOptions opts;
  return CFileReader::Open(env, path, opts, new_reader);
}

////////////////////////////////////////////////////////////
// CFile Base
////////////////////////////////////////////////////////////

CFileSet::CFileSet(Env *env,
                             const string &dir,
                             const Schema &schema) :
  env_(env),
  dir_(dir),
  schema_(schema)
{}

CFileSet::~CFileSet() {
}


Status CFileSet::OpenAllColumns() {
  return OpenColumns(schema_.num_columns());
}

Status CFileSet::OpenKeyColumns() {
  return OpenColumns(schema_.num_key_columns());
}

Status CFileSet::OpenColumns(size_t num_cols) {
  CHECK_LE(num_cols, schema_.num_columns());

  RETURN_NOT_OK( OpenBloomReader() );

  readers_.resize(num_cols);

  for (int i = 0; i < num_cols; i++) {
    if (readers_[i] != NULL) {
      // Already open.
      continue;
    }

    gscoped_ptr<CFileReader> reader;
    RETURN_NOT_OK(OpenReader(env_, dir_, i, &reader));
    readers_[i].reset(reader.release());
    LOG(INFO) << "Successfully opened cfile for column " <<
      schema_.column(i).ToString() << " in " << dir_;;
  }

  return Status::OK();
}


Status CFileSet::OpenBloomReader() {
  if (bloom_reader_ != NULL) {
    return Status::OK();
  }

  Status s = BloomFileReader::Open(env_, Layer::GetBloomPath(dir_), &bloom_reader_);
  if (!s.ok()) {
    LOG(WARNING) << "Unable to open bloom file in " << dir_ << ": "
                 << s.ToString();
    // Continue without bloom.
  }

  return Status::OK();
}


Status CFileSet::NewColumnIterator(size_t col_idx, CFileIterator **iter) const {
  CHECK_LT(col_idx, readers_.size());

  return CHECK_NOTNULL(readers_[col_idx].get())->NewIterator(iter);
}


CFileSet::Iterator *CFileSet::NewIterator(const Schema &projection) const {
  return new CFileSet::Iterator(shared_from_this(), projection);
}

Status CFileSet::CountRows(size_t *count) const {
  const shared_ptr<cfile::CFileReader> &reader = readers_[0];
  return reader->CountRows(count);
}

uint64_t CFileSet::EstimateOnDiskSize() const {
  uint64_t ret = 0;
  BOOST_FOREACH(const shared_ptr<CFileReader> &reader, readers_) {
    ret += reader->file_size();
  }
  return ret;
}

Status CFileSet::FindRow(const void *key, uint32_t *idx) const {
  CFileIterator *key_iter;
  RETURN_NOT_OK( NewColumnIterator(0, &key_iter) );
  gscoped_ptr<CFileIterator> key_iter_scoped(key_iter); // free on return

  // TODO: check bloom filter

  bool exact;
  RETURN_NOT_OK( key_iter->SeekAtOrAfter(key, &exact) );
  if (!exact) {
    return Status::NotFound("not present in storefile (failed seek)");
  }

  *idx = key_iter->GetCurrentOrdinal();
  return Status::OK();
}

Status CFileSet::CheckRowPresent(const LayerKeyProbe &probe, bool *present) const {
  if (bloom_reader_ != NULL && FLAGS_consult_bloom_filters) {
    Status s = bloom_reader_->CheckKeyPresent(probe.bloom_probe(), present);
    if (s.ok() && !*present) {
      return Status::OK();
    } else if (!s.ok()) {
      LOG(WARNING) << "Unable to query bloom: " << s.ToString()
                   << " (disabling bloom for this layer from this point forward)";
      const_cast<CFileSet *>(this)->bloom_reader_.reset(NULL);
      // Continue with the slow path
    }
  }

  uint32_t junk;
  Status s = FindRow(probe.raw_key(), &junk);
  if (s.IsNotFound()) {
    // In the case that the key comes past the end of the file, Seek
    // will return NotFound. In that case, it is OK from this function's
    // point of view - just a non-present key.
    *present = false;
    return Status::OK();
  }
  *present = true;
  return s;
}


////////////////////////////////////////////////////////////
// Iterator
////////////////////////////////////////////////////////////

Status CFileSet::Iterator::Init(ScanSpec *spec) {
  CHECK(!initted_);

  RETURN_NOT_OK(projection_.GetProjectionFrom(
                  base_data_->schema(), &projection_mapping_));

  // Setup Key Iterator.

  // Only support single key column for now.
  CHECK_EQ(base_data_->schema().num_key_columns(), 1);
  int key_col = 0;

  CFileIterator *tmp;
  RETURN_NOT_OK(base_data_->NewColumnIterator(key_col, &tmp));
  key_iter_.reset(tmp);

  // Setup column iterators.

  for (size_t i = 0; i < projection_.num_columns(); i++) {
    size_t col_in_layer = projection_mapping_[i];

    CFileIterator *iter;
    RETURN_NOT_OK(base_data_->NewColumnIterator(col_in_layer, &iter));
    col_iters_.push_back(iter);
  }

  initted_ = true;

  // TODO: later, Init() will probably take some kind of predicate,
  // which would tell us where to seek to.
  return SeekToOrdinal(0);
}

Status CFileSet::Iterator::SeekToOrdinal(uint32_t ord_idx) {
  DCHECK(initted_);
  BOOST_FOREACH(CFileIterator &col_iter, col_iters_) {
    RETURN_NOT_OK(col_iter.SeekToOrdinal(ord_idx));
  }

  cur_idx_ = 0;

  Unprepare();

  return Status::OK();
}


void CFileSet::Iterator::Unprepare() {
  prepared_count_ = 0;
  cols_prepared_.assign(col_iters_.size(), false);
}

Status CFileSet::Iterator::PrepareBatch(size_t *n) {
  DCHECK_EQ(prepared_count_, 0) << "Already prepared";

  size_t remaining = row_count_ - cur_idx_;
  if (*n > remaining) {
    *n = remaining;
  }

  prepared_count_ = *n;

  // Lazily prepare the first column when it is materialized.
  return Status::OK();
}


Status CFileSet::Iterator::PrepareColumn(size_t idx) {
  if (cols_prepared_[idx]) {
    // Already prepared in this batch.
    return Status::OK();
  }

  CFileIterator &col_iter = col_iters_[idx];
  size_t n = prepared_count_;

  if (col_iter.GetCurrentOrdinal() != cur_idx_) {
    // This column must have not been materialized in a prior block.
    // We need to seek it to the correct offset.
    RETURN_NOT_OK(col_iter.SeekToOrdinal(cur_idx_));
  }

  Status s = col_iter.PrepareBatch(&n);
  if (!s.ok()) {
    LOG(WARNING) << "Unable to prepare column " << idx << ": " << s.ToString();
    return s;
  }

  if (n != prepared_count_) {
    return Status::Corruption(
      StringPrintf("Column %zd (%s) didn't yield enough rows at offset %zd: expected "
                   "%zd but only got %zd", idx, projection_.column(idx).ToString().c_str(),
                   cur_idx_, prepared_count_, n));
  }

  cols_prepared_[idx] = true;

  return Status::OK();
}

Status CFileSet::Iterator::MaterializeColumn(size_t col_idx, ColumnBlock *dst) {
  CHECK_GT(prepared_count_, 0);
  DCHECK_LT(col_idx, col_iters_.size());

  RETURN_NOT_OK(PrepareColumn(col_idx));
  CFileIterator &iter = col_iters_[col_idx];
  return iter.Scan(dst);
}

Status CFileSet::Iterator::FinishBatch() {
  CHECK_GT(prepared_count_, 0);

  for (size_t i = 0; i < col_iters_.size(); i++) {
    if (cols_prepared_[i]) {
      Status s = col_iters_[i].FinishBatch();
      if (!s.ok()) {
        LOG(WARNING) << "Unable to FinishBatch() on column " << i;
        return s;
      }
    }
  }

  cur_idx_ += prepared_count_;
  Unprepare();

  return Status::OK();
}


void CFileSet::Iterator::GetIOStatistics(vector<CFileIterator::IOStatistics> *stats) {
  stats->clear();
  stats->reserve(col_iters_.size());
  BOOST_FOREACH(const CFileIterator &iter, col_iters_) {
    stats->push_back(iter.io_statistics());
  }
}

} // namespace tablet
} // namespace kudu

