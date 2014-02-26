//  Copyright (c) 2013, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include <algorithm>
#include <vector>
#include <string>

#include "db/db_impl.h"
#include "rocksdb/env.h"
#include "rocksdb/db.h"
#include "util/testharness.h"
#include "util/testutil.h"
#include "utilities/merge_operators.h"

namespace rocksdb {

namespace {
std::string RandomString(Random* rnd, int len) {
  std::string r;
  test::RandomString(rnd, len, &r);
  return r;
}
}  // anonymous namespace

class ColumnFamilyTest {
 public:
  ColumnFamilyTest() : rnd_(139) {
    env_ = Env::Default();
    dbname_ = test::TmpDir() + "/column_family_test";
    db_options_.create_if_missing = true;
    DestroyDB(dbname_, Options(db_options_, column_family_options_));
  }

  void Close() {
    for (auto h : handles_) {
      delete h;
    }
    handles_.clear();
    names_.clear();
    delete db_;
    db_ = nullptr;
  }

  Status TryOpen(std::vector<std::string> cf,
                 std::vector<ColumnFamilyOptions> options = {}) {
    std::vector<ColumnFamilyDescriptor> column_families;
    names_.clear();
    for (size_t i = 0; i < cf.size(); ++i) {
      column_families.push_back(ColumnFamilyDescriptor(
          cf[i], options.size() == 0 ? column_family_options_ : options[i]));
      names_.push_back(cf[i]);
    }
    return DB::Open(db_options_, dbname_, column_families, &handles_, &db_);
  }

  void Open(std::vector<std::string> cf,
            std::vector<ColumnFamilyOptions> options = {}) {
    ASSERT_OK(TryOpen(cf, options));
  }

  void Open() {
    Open({"default"});
  }

  DBImpl* dbfull() { return reinterpret_cast<DBImpl*>(db_); }

  int GetProperty(int cf, std::string property) {
    std::string value;
    ASSERT_TRUE(dbfull()->GetProperty(handles_[cf], property, &value));
    return std::stoi(value);
  }

  void Destroy() {
    for (auto h : handles_) {
      delete h;
    }
    handles_.clear();
    names_.clear();
    delete db_;
    db_ = nullptr;
    ASSERT_OK(DestroyDB(dbname_, Options(db_options_, column_family_options_)));
  }

  void CreateColumnFamilies(
      const std::vector<std::string>& cfs,
      const std::vector<ColumnFamilyOptions> options = {}) {
    int cfi = handles_.size();
    handles_.resize(cfi + cfs.size());
    names_.resize(cfi + cfs.size());
    for (size_t i = 0; i < cfs.size(); ++i) {
      ASSERT_OK(db_->CreateColumnFamily(
          options.size() == 0 ? column_family_options_ : options[i], cfs[i],
          &handles_[cfi]));
      names_[cfi] = cfs[i];
      cfi++;
    }
  }

  void Reopen(const std::vector<ColumnFamilyOptions> options = {}) {
    std::vector<std::string> names;
    for (auto name : names_) {
      if (name != "") {
        names.push_back(name);
      }
    }
    Close();
    assert(options.size() == 0 || names.size() == options.size());
    Open(names, options);
  }

  void CreateColumnFamiliesAndReopen(const std::vector<std::string>& cfs) {
    CreateColumnFamilies(cfs);
    Reopen();
  }

  void DropColumnFamilies(const std::vector<int>& cfs) {
    for (auto cf : cfs) {
      ASSERT_OK(db_->DropColumnFamily(handles_[cf]));
      delete handles_[cf];
      handles_[cf] = nullptr;
      names_[cf] = "";
    }
  }

  void PutRandomData(int cf, int num, int key_value_size) {
    for (int i = 0; i < num; ++i) {
      // 10 bytes for key, rest is value
      ASSERT_OK(Put(cf, test::RandomKey(&rnd_, 10),
                    RandomString(&rnd_, key_value_size - 10)));
    }
  }

  void WaitForFlush(int cf) {
    ASSERT_OK(dbfull()->TEST_WaitForFlushMemTable(handles_[cf]));
  }

  Status Put(int cf, const std::string& key, const std::string& value) {
    return db_->Put(WriteOptions(), handles_[cf], Slice(key), Slice(value));
  }
  Status Merge(int cf, const std::string& key, const std::string& value) {
    return db_->Merge(WriteOptions(), handles_[cf], Slice(key), Slice(value));
  }
  Status Flush(int cf) {
    return db_->Flush(FlushOptions(), handles_[cf]);
  }

  std::string Get(int cf, const std::string& key) {
    ReadOptions options;
    options.verify_checksums = true;
    std::string result;
    Status s = db_->Get(options, handles_[cf], Slice(key), &result);
    if (s.IsNotFound()) {
      result = "NOT_FOUND";
    } else if (!s.ok()) {
      result = s.ToString();
    }
    return result;
  }

  void Compact(int cf, const Slice& start, const Slice& limit) {
    ASSERT_OK(db_->CompactRange(handles_[cf], &start, &limit));
  }

  int NumTableFilesAtLevel(int cf, int level) {
    std::string property;
    ASSERT_TRUE(db_->GetProperty(
        handles_[cf], "rocksdb.num-files-at-level" + NumberToString(level),
        &property));
    return atoi(property.c_str());
  }

  // Return spread of files per level
  std::string FilesPerLevel(int cf) {
    std::string result;
    int last_non_zero_offset = 0;
    for (int level = 0; level < column_family_options_.num_levels; level++) {
      int f = NumTableFilesAtLevel(cf, level);
      char buf[100];
      snprintf(buf, sizeof(buf), "%s%d", (level ? "," : ""), f);
      result += buf;
      if (f > 0) {
        last_non_zero_offset = result.size();
      }
    }
    result.resize(last_non_zero_offset);
    return result;
  }

  int CountLiveFiles(int cf) {
    std::vector<LiveFileMetaData> metadata;
    db_->GetLiveFilesMetaData(&metadata);
    return static_cast<int>(metadata.size());
  }

  // Do n memtable flushes, each of which produces an sstable
  // covering the range [small,large].
  void MakeTables(int cf, int n, const std::string& small,
                  const std::string& large) {
    for (int i = 0; i < n; i++) {
      ASSERT_OK(Put(cf, small, "begin"));
      ASSERT_OK(Put(cf, large, "end"));
      ASSERT_OK(db_->Flush(FlushOptions(), handles_[cf]));
    }
  }

  int CountLiveLogFiles() {
    int ret = 0;
    VectorLogPtr wal_files;
    ASSERT_OK(db_->GetSortedWalFiles(wal_files));
    for (const auto& wal : wal_files) {
      if (wal->Type() == kAliveLogFile) {
        ++ret;
      }
    }
    return ret;
  }

  void AssertNumberOfImmutableMemtables(std::vector<int> num_per_cf) {
    assert(num_per_cf.size() == handles_.size());

    for (size_t i = 0; i < num_per_cf.size(); ++i) {
      ASSERT_EQ(num_per_cf[i],
                GetProperty(i, "rocksdb.num-immutable-mem-table"));
    }
  }

  void CopyFile(const std::string& source, const std::string& destination,
                uint64_t size = 0) {
    const EnvOptions soptions;
    unique_ptr<SequentialFile> srcfile;
    ASSERT_OK(env_->NewSequentialFile(source, &srcfile, soptions));
    unique_ptr<WritableFile> destfile;
    ASSERT_OK(env_->NewWritableFile(destination, &destfile, soptions));

    if (size == 0) {
      // default argument means copy everything
      ASSERT_OK(env_->GetFileSize(source, &size));
    }

    char buffer[4096];
    Slice slice;
    while (size > 0) {
      uint64_t one = std::min(uint64_t(sizeof(buffer)), size);
      ASSERT_OK(srcfile->Read(one, &slice, buffer));
      ASSERT_OK(destfile->Append(slice));
      size -= slice.size();
    }
    ASSERT_OK(destfile->Close());
  }

  std::vector<ColumnFamilyHandle*> handles_;
  std::vector<std::string> names_;
  ColumnFamilyOptions column_family_options_;
  DBOptions db_options_;
  std::string dbname_;
  DB* db_ = nullptr;
  Env* env_;
  Random rnd_;
};

TEST(ColumnFamilyTest, AddDrop) {
  Open();
  CreateColumnFamilies({"one", "two", "three"});
  DropColumnFamilies({2});
  CreateColumnFamilies({"four"});
  Close();
  ASSERT_TRUE(TryOpen({"default"}).IsInvalidArgument());
  Open({"default", "one", "three", "four"});
  DropColumnFamilies({1});
  Reopen();
  Close();

  std::vector<std::string> families;
  ASSERT_OK(DB::ListColumnFamilies(db_options_, dbname_, &families));
  sort(families.begin(), families.end());
  ASSERT_TRUE(families ==
              std::vector<std::string>({"default", "four", "three"}));
}

TEST(ColumnFamilyTest, DropTest) {
  // first iteration - dont reopen DB before dropping
  // second iteration - reopen DB before dropping
  for (int iter = 0; iter < 2; ++iter) {
    Open({"default"});
    CreateColumnFamiliesAndReopen({"pikachu"});
    for (int i = 0; i < 100; ++i) {
      ASSERT_OK(Put(1, std::to_string(i), "bar" + std::to_string(i)));
    }
    ASSERT_OK(Flush(1));

    if (iter == 1) {
      Reopen();
    }
    ASSERT_EQ("bar1", Get(1, "1"));

    ASSERT_EQ(CountLiveFiles(1), 1);
    DropColumnFamilies({1});
    // make sure that all files are deleted when we drop the column family
    ASSERT_EQ(CountLiveFiles(1), 0);
    Destroy();
  }
}

TEST(ColumnFamilyTest, WriteBatchFailure) {
  Open();
  WriteBatch batch;
  batch.Put(1, Slice("non-existing"), Slice("column-family"));
  Status s = db_->Write(WriteOptions(), &batch);
  ASSERT_TRUE(s.IsInvalidArgument());
  CreateColumnFamilies({"one"});
  ASSERT_OK(db_->Write(WriteOptions(), &batch));
  Close();
}

TEST(ColumnFamilyTest, ReadWrite) {
  Open();
  CreateColumnFamiliesAndReopen({"one", "two"});
  ASSERT_OK(Put(0, "foo", "v1"));
  ASSERT_OK(Put(0, "bar", "v2"));
  ASSERT_OK(Put(1, "mirko", "v3"));
  ASSERT_OK(Put(0, "foo", "v2"));
  ASSERT_OK(Put(2, "fodor", "v5"));

  for (int iter = 0; iter <= 3; ++iter) {
    ASSERT_EQ("v2", Get(0, "foo"));
    ASSERT_EQ("v2", Get(0, "bar"));
    ASSERT_EQ("v3", Get(1, "mirko"));
    ASSERT_EQ("v5", Get(2, "fodor"));
    ASSERT_EQ("NOT_FOUND", Get(0, "fodor"));
    ASSERT_EQ("NOT_FOUND", Get(1, "fodor"));
    ASSERT_EQ("NOT_FOUND", Get(2, "foo"));
    if (iter <= 1) {
      Reopen();
    }
  }
  Close();
}

TEST(ColumnFamilyTest, IgnoreRecoveredLog) {
  std::string backup_logs = dbname_ + "/backup_logs";

  // delete old files in backup_logs directory
  ASSERT_OK(env_->CreateDirIfMissing(dbname_));
  ASSERT_OK(env_->CreateDirIfMissing(backup_logs));
  std::vector<std::string> old_files;
  env_->GetChildren(backup_logs, &old_files);
  for (auto& file : old_files) {
    if (file != "." && file != "..") {
      env_->DeleteFile(backup_logs + "/" + file);
    }
  }

  column_family_options_.merge_operator =
      MergeOperators::CreateUInt64AddOperator();
  db_options_.wal_dir = dbname_ + "/logs";
  Destroy();
  Open();
  CreateColumnFamilies({"cf1", "cf2"});

  // fill up the DB
  std::string one, two, three;
  PutFixed64(&one, 1);
  PutFixed64(&two, 2);
  PutFixed64(&three, 3);
  ASSERT_OK(Merge(0, "foo", one));
  ASSERT_OK(Merge(1, "mirko", one));
  ASSERT_OK(Merge(0, "foo", one));
  ASSERT_OK(Merge(2, "bla", one));
  ASSERT_OK(Merge(2, "fodor", one));
  ASSERT_OK(Merge(0, "bar", one));
  ASSERT_OK(Merge(2, "bla", one));
  ASSERT_OK(Merge(1, "mirko", two));
  ASSERT_OK(Merge(1, "franjo", one));

  // copy the logs to backup
  std::vector<std::string> logs;
  env_->GetChildren(db_options_.wal_dir, &logs);
  for (auto& log : logs) {
    if (log != ".." && log != ".") {
      CopyFile(db_options_.wal_dir + "/" + log, backup_logs + "/" + log);
    }
  }

  // recover the DB
  Close();

  // 1. check consistency
  // 2. copy the logs from backup back to WAL dir. if the recovery happens
  // again on the same log files, this should lead to incorrect results
  // due to applying merge operator twice
  // 3. check consistency
  for (int iter = 0; iter < 2; ++iter) {
    // assert consistency
    Open({"default", "cf1", "cf2"});
    ASSERT_EQ(two, Get(0, "foo"));
    ASSERT_EQ(one, Get(0, "bar"));
    ASSERT_EQ(three, Get(1, "mirko"));
    ASSERT_EQ(one, Get(1, "franjo"));
    ASSERT_EQ(one, Get(2, "fodor"));
    ASSERT_EQ(two, Get(2, "bla"));
    Close();

    if (iter == 0) {
      // copy the logs from backup back to wal dir
      for (auto& log : logs) {
        if (log != ".." && log != ".") {
          CopyFile(backup_logs + "/" + log, db_options_.wal_dir + "/" + log);
        }
      }
    }
  }
}

TEST(ColumnFamilyTest, FlushTest) {
  Open();
  CreateColumnFamiliesAndReopen({"one", "two"});
  ASSERT_OK(Put(0, "foo", "v1"));
  ASSERT_OK(Put(0, "bar", "v2"));
  ASSERT_OK(Put(1, "mirko", "v3"));
  ASSERT_OK(Put(0, "foo", "v2"));
  ASSERT_OK(Put(2, "fodor", "v5"));
  for (int i = 0; i < 3; ++i) {
    Flush(i);
  }
  Reopen();

  for (int iter = 0; iter <= 2; ++iter) {
    ASSERT_EQ("v2", Get(0, "foo"));
    ASSERT_EQ("v2", Get(0, "bar"));
    ASSERT_EQ("v3", Get(1, "mirko"));
    ASSERT_EQ("v5", Get(2, "fodor"));
    ASSERT_EQ("NOT_FOUND", Get(0, "fodor"));
    ASSERT_EQ("NOT_FOUND", Get(1, "fodor"));
    ASSERT_EQ("NOT_FOUND", Get(2, "foo"));
    if (iter <= 1) {
      Reopen();
    }
  }
  Close();
}

// Makes sure that obsolete log files get deleted
TEST(ColumnFamilyTest, LogDeletionTest) {
  column_family_options_.write_buffer_size = 100000;  // 100KB
  Open();
  CreateColumnFamilies({"one", "two", "three", "four"});
  // Each bracket is one log file. if number is in (), it means
  // we don't need it anymore (it's been flushed)
  // []
  ASSERT_EQ(CountLiveLogFiles(), 0);
  PutRandomData(0, 1, 100);
  // [0]
  PutRandomData(1, 1, 100);
  // [0, 1]
  PutRandomData(1, 1000, 100);
  WaitForFlush(1);
  // [0, (1)] [1]
  ASSERT_EQ(CountLiveLogFiles(), 2);
  PutRandomData(0, 1, 100);
  // [0, (1)] [0, 1]
  ASSERT_EQ(CountLiveLogFiles(), 2);
  PutRandomData(2, 1, 100);
  // [0, (1)] [0, 1, 2]
  PutRandomData(2, 1000, 100);
  WaitForFlush(2);
  // [0, (1)] [0, 1, (2)] [2]
  ASSERT_EQ(CountLiveLogFiles(), 3);
  PutRandomData(2, 1000, 100);
  WaitForFlush(2);
  // [0, (1)] [0, 1, (2)] [(2)] [2]
  ASSERT_EQ(CountLiveLogFiles(), 4);
  PutRandomData(3, 1, 100);
  // [0, (1)] [0, 1, (2)] [(2)] [2, 3]
  PutRandomData(1, 1, 100);
  // [0, (1)] [0, 1, (2)] [(2)] [1, 2, 3]
  ASSERT_EQ(CountLiveLogFiles(), 4);
  PutRandomData(1, 1000, 100);
  WaitForFlush(1);
  // [0, (1)] [0, (1), (2)] [(2)] [(1), 2, 3] [1]
  ASSERT_EQ(CountLiveLogFiles(), 5);
  PutRandomData(0, 1000, 100);
  WaitForFlush(0);
  // [(0), (1)] [(0), (1), (2)] [(2)] [(1), 2, 3] [1, (0)] [0]
  // delete obsolete logs -->
  // [(1), 2, 3] [1, (0)] [0]
  ASSERT_EQ(CountLiveLogFiles(), 3);
  PutRandomData(0, 1000, 100);
  WaitForFlush(0);
  // [(1), 2, 3] [1, (0)], [(0)] [0]
  ASSERT_EQ(CountLiveLogFiles(), 4);
  PutRandomData(1, 1000, 100);
  WaitForFlush(1);
  // [(1), 2, 3] [(1), (0)] [(0)] [0, (1)] [1]
  ASSERT_EQ(CountLiveLogFiles(), 5);
  PutRandomData(2, 1000, 100);
  WaitForFlush(2);
  // [(1), (2), 3] [(1), (0)] [(0)] [0, (1)] [1, (2)], [2]
  ASSERT_EQ(CountLiveLogFiles(), 6);
  PutRandomData(3, 1000, 100);
  WaitForFlush(3);
  // [(1), (2), (3)] [(1), (0)] [(0)] [0, (1)] [1, (2)], [2, (3)] [3]
  // delete obsolete logs -->
  // [0, (1)] [1, (2)], [2, (3)] [3]
  ASSERT_EQ(CountLiveLogFiles(), 4);
  Close();
}

// Makes sure that obsolete log files get deleted
TEST(ColumnFamilyTest, DifferentWriteBufferSizes) {
  Open();
  CreateColumnFamilies({"one", "two", "three"});
  ColumnFamilyOptions default_cf, one, two, three;
  // setup options. all column families have max_write_buffer_number setup to 10
  // "default" -> 100KB memtable, start flushing immediatelly
  // "one" -> 200KB memtable, start flushing with two immutable memtables
  // "two" -> 1MB memtable, start flushing with three immutable memtables
  // "three" -> 90KB memtable, start flushing with four immutable memtables
  default_cf.write_buffer_size = 100000;
  default_cf.max_write_buffer_number = 10;
  default_cf.min_write_buffer_number_to_merge = 1;
  one.write_buffer_size = 200000;
  one.max_write_buffer_number = 10;
  one.min_write_buffer_number_to_merge = 2;
  two.write_buffer_size = 1000000;
  two.max_write_buffer_number = 10;
  two.min_write_buffer_number_to_merge = 3;
  three.write_buffer_size = 90000;
  three.max_write_buffer_number = 10;
  three.min_write_buffer_number_to_merge = 4;

  Reopen({default_cf, one, two, three});

  int micros_wait_for_flush = 100000;
  PutRandomData(0, 100, 1000);
  env_->SleepForMicroseconds(micros_wait_for_flush);
  AssertNumberOfImmutableMemtables({0, 0, 0, 0});
  ASSERT_EQ(CountLiveLogFiles(), 1);
  PutRandomData(1, 200, 1000);
  env_->SleepForMicroseconds(micros_wait_for_flush);
  AssertNumberOfImmutableMemtables({0, 1, 0, 0});
  ASSERT_EQ(CountLiveLogFiles(), 2);
  PutRandomData(2, 1000, 1000);
  env_->SleepForMicroseconds(micros_wait_for_flush);
  AssertNumberOfImmutableMemtables({0, 1, 1, 0});
  ASSERT_EQ(CountLiveLogFiles(), 3);
  PutRandomData(2, 1000, 1000);
  env_->SleepForMicroseconds(micros_wait_for_flush);
  AssertNumberOfImmutableMemtables({0, 1, 2, 0});
  ASSERT_EQ(CountLiveLogFiles(), 4);
  PutRandomData(3, 90, 1000);
  env_->SleepForMicroseconds(micros_wait_for_flush);
  AssertNumberOfImmutableMemtables({0, 1, 2, 1});
  ASSERT_EQ(CountLiveLogFiles(), 5);
  PutRandomData(3, 90, 1000);
  env_->SleepForMicroseconds(micros_wait_for_flush);
  AssertNumberOfImmutableMemtables({0, 1, 2, 2});
  ASSERT_EQ(CountLiveLogFiles(), 6);
  PutRandomData(3, 90, 1000);
  env_->SleepForMicroseconds(micros_wait_for_flush);
  AssertNumberOfImmutableMemtables({0, 1, 2, 3});
  ASSERT_EQ(CountLiveLogFiles(), 7);
  PutRandomData(0, 100, 1000);
  env_->SleepForMicroseconds(micros_wait_for_flush);
  AssertNumberOfImmutableMemtables({0, 1, 2, 3});
  ASSERT_EQ(CountLiveLogFiles(), 8);
  PutRandomData(2, 100, 10000);
  env_->SleepForMicroseconds(micros_wait_for_flush);
  AssertNumberOfImmutableMemtables({0, 1, 0, 3});
  ASSERT_EQ(CountLiveLogFiles(), 9);
  PutRandomData(3, 90, 1000);
  env_->SleepForMicroseconds(micros_wait_for_flush);
  AssertNumberOfImmutableMemtables({0, 1, 0, 0});
  ASSERT_EQ(CountLiveLogFiles(), 10);
  PutRandomData(3, 90, 1000);
  env_->SleepForMicroseconds(micros_wait_for_flush);
  AssertNumberOfImmutableMemtables({0, 1, 0, 1});
  ASSERT_EQ(CountLiveLogFiles(), 11);
  PutRandomData(1, 200, 1000);
  env_->SleepForMicroseconds(micros_wait_for_flush);
  AssertNumberOfImmutableMemtables({0, 0, 0, 1});
  ASSERT_EQ(CountLiveLogFiles(), 5);
  PutRandomData(3, 90*6, 1000);
  env_->SleepForMicroseconds(micros_wait_for_flush);
  AssertNumberOfImmutableMemtables({0, 0, 0, 0});
  ASSERT_EQ(CountLiveLogFiles(), 12);
  PutRandomData(0, 100, 1000);
  env_->SleepForMicroseconds(micros_wait_for_flush);
  AssertNumberOfImmutableMemtables({0, 0, 0, 0});
  ASSERT_EQ(CountLiveLogFiles(), 12);
  PutRandomData(2, 3*100, 10000);
  env_->SleepForMicroseconds(micros_wait_for_flush);
  AssertNumberOfImmutableMemtables({0, 0, 0, 0});
  ASSERT_EQ(CountLiveLogFiles(), 12);
  PutRandomData(1, 2*200, 1000);
  env_->SleepForMicroseconds(micros_wait_for_flush);
  AssertNumberOfImmutableMemtables({0, 0, 0, 0});
  ASSERT_EQ(CountLiveLogFiles(), 7);
  Close();
}

}  // namespace rocksdb

int main(int argc, char** argv) {
  return rocksdb::test::RunAllTests();
}