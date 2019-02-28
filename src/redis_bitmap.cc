#include "redis_bitmap.h"

const uint32_t kBitmapSegmentBits = 1024 * 8;
const uint32_t kBitmapSegmentBytes = 1024;

uint32_t kNum2Bits[256] = {
    0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    4, 5, 5, 6, 5, 6, 6, 7, 5, 6, 6, 7, 6, 7, 7, 8
};

rocksdb::Status RedisBitmap::GetMetadata(Slice key, BitmapMetadata*metadata) {
  return RedisDB::GetMetadata(kRedisBitmap, key, metadata);
}

rocksdb::Status RedisBitmap::GetBit(Slice key, uint32_t offset, bool *bit) {
  *bit = false;
  std::string ns_key;
  AppendNamespacePrefix(key, &ns_key);
  key = Slice(ns_key);

  BitmapMetadata metadata;
  rocksdb::Status s = GetMetadata(key, &metadata);
  if (!s.ok()) return s.IsNotFound() ? rocksdb::Status::OK() : s;

  LatestSnapShot ss(db_);
  rocksdb::ReadOptions read_options;
  read_options.snapshot = ss.GetSnapShot();
  uint32_t index = (offset/kBitmapSegmentBits)*kBitmapSegmentBytes;
  std::string sub_key, value;
  InternalKey(key, std::to_string(index), metadata.version).Encode(&sub_key);
  s = db_->Get(read_options, sub_key, &value);
  if (!s.ok()) return s.IsNotFound() ? rocksdb::Status::OK() : s;
  uint32_t byte_index = (offset/8) % kBitmapSegmentBytes;
  if ((byte_index < value.size() && (value[byte_index]>>(offset%8)))) {
    *bit = true;
  }
  return rocksdb::Status::OK();
}

rocksdb::Status RedisBitmap::SetBit(Slice key, uint32_t offset, bool new_bit, bool *old_bit) {
  std::string ns_key;
  AppendNamespacePrefix(key, &ns_key);
  key = Slice(ns_key);

  LockGuard guard(storage_->GetLockManager(), key);
  BitmapMetadata metadata;
  rocksdb::Status s = GetMetadata(key, &metadata);
  if (!s.ok() && !s.IsNotFound()) return s;

  std::string sub_key, value;
  uint32_t index = (offset/kBitmapSegmentBits)*kBitmapSegmentBytes;
  InternalKey(key, std::to_string(index), metadata.version).Encode(&sub_key);
  if (s.ok()) {
    s = db_->Get(rocksdb::ReadOptions(), sub_key, &value);
    if (!s.ok() && !s.IsNotFound()) return s;
  }
  uint32_t byte_index = (offset/8)%kBitmapSegmentBytes;
  uint32_t bitmap_size = metadata.size;
  if (byte_index >= value.size()) {  // expand the bitmap
    size_t expand_size;
    if (byte_index >= value.size() * 2) {
      expand_size = byte_index-value.size()+1;
    } else {
      expand_size = value.size();
    }
    value.append(expand_size, 0);
    if (value.size()+index > bitmap_size) {
      bitmap_size = static_cast<uint32_t>(value.size()) + index;
    }
  }
  uint32_t bit_offset = offset%8;
  *old_bit = (value[byte_index] & (1 << bit_offset)) != 0;
  if (new_bit) {
    value[byte_index] |= 1 << bit_offset;
  } else {
    value[byte_index] &= ~(1 << bit_offset);
  }

  rocksdb::WriteBatch batch;
  batch.Put(sub_key, value);
  if (metadata.size != bitmap_size) {
    metadata.size = bitmap_size;
    std::string bytes;
    metadata.Encode(&bytes);
    batch.Put(metadata_cf_handle_, key, bytes);
  }
  return storage_->Write(rocksdb::WriteOptions(), &batch);
}

rocksdb::Status RedisBitmap::BitCount(Slice key, int start, int stop, uint32_t *cnt) {
  *cnt = 0;

  std::string ns_key;
  AppendNamespacePrefix(key, &ns_key);
  key = Slice(ns_key);
  BitmapMetadata metadata;
  rocksdb::Status s = GetMetadata(key, &metadata);
  if (!s.ok()) return s.IsNotFound() ? rocksdb::Status::OK() : s;

  if (start < 0) start += metadata.size;
  if (stop < 0) stop += metadata.size;
  if (stop > static_cast<int>(metadata.size)) stop = metadata.size;
  if (start < 0 || stop <= 0 || start >= stop) return rocksdb::Status::OK();

  LatestSnapShot ss(db_);
  rocksdb::ReadOptions read_options;
  read_options.snapshot = ss.GetSnapShot();
  int start_index = start/kBitmapSegmentBytes;
  int stop_index = stop/kBitmapSegmentBytes;
  // Don't use multi get to prevent large range query, and take too much memory
  std::string sub_key, value;
  for (int i = start_index; i <= stop_index; i++) {
    InternalKey(key, std::to_string(i*kBitmapSegmentBytes), metadata.version).Encode(&sub_key);
    s = db_->Get(read_options, sub_key, &value);
    if (!s.ok()&&!s.IsNotFound()) return s;
    if (s.IsNotFound()) continue;
    size_t j = 0;
    if (i == start_index) j = start % kBitmapSegmentBytes;
    for (; j < value.size(); j++) {
      if (i == stop_index && j > (stop % kBitmapSegmentBytes)) break;
      *cnt += kNum2Bits[static_cast<int>(value[j])];
    }
  }
  return rocksdb::Status::OK();
}

rocksdb::Status RedisBitmap::BitPos(Slice key, bool bit, int start, int stop, int *pos) {
  std::string ns_key;
  AppendNamespacePrefix(key, &ns_key);
  key = Slice(ns_key);
  BitmapMetadata metadata;
  rocksdb::Status s = GetMetadata(key, &metadata);
  if (!s.ok() && !s.IsNotFound()) return s;
  if (s.IsNotFound()) {
    *pos = bit ? -1 : 0;
    return rocksdb::Status::OK();
  }
  if (start < 0) start += metadata.size;
  if (stop < 0) stop += metadata.size;
  if (start < 0 || stop < 0 || start > stop) {
    *pos = -1;
    return rocksdb::Status::OK();
  }

  auto bitPosInByte = [](char byte, bool bit) -> int {
    for (int i = 0; i < 8; i++) {
      if (bit && (byte & (1 << i)) != 0) return i;
      if (!bit && (byte & (1 << i)) == 0) return i;
    }
    return -1;
  };

  LatestSnapShot ss(db_);
  rocksdb::ReadOptions read_options;
  read_options.snapshot = ss.GetSnapShot();
  int start_index = start/kBitmapSegmentBytes;
  int stop_index = stop/kBitmapSegmentBytes;
  // Don't use multi get to prevent large range query, and take too much memory
  std::string sub_key, value;
  for (int i = start_index; i <= stop_index; i++) {
    InternalKey(key, std::to_string(i*kBitmapSegmentBytes), metadata.version).Encode(&sub_key);
    s = db_->Get(read_options, sub_key, &value);
    if (!s.ok()&&!s.IsNotFound()) return s;
    if (s.IsNotFound()) {
      if (!bit) {
        *pos =  i * kBitmapSegmentBits;
        return rocksdb::Status::OK();
      }
      continue;
    }
    size_t j = 0;
    if (i == start_index) j = start % kBitmapSegmentBytes;
    for (; j < value.size(); j++) {
      if (i == stop_index && j > (stop%kBitmapSegmentBytes)) break;
      if (bitPosInByte(value[j], bit) != -1) {
        *pos = static_cast<int>(i*kBitmapSegmentBits + j*8 + bitPosInByte(value[j], bit));
        return rocksdb::Status::OK();
      }
    }
    if (!bit && value.size() < kBitmapSegmentBytes) {
      *pos = static_cast<int>(i * kBitmapSegmentBits + value.size()*8);
      return rocksdb::Status::OK();
    }
  }
  // bit was not found
  *pos = bit ? -1 : static_cast<int>(metadata.size * 8);
  return rocksdb::Status::OK();
}