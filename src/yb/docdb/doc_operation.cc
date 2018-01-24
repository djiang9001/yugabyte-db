// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include "yb/common/partition.h"
#include "yb/common/ql_scanspec.h"
#include "yb/common/ql_storage_interface.h"
#include "yb/common/ql_value.h"
#include "yb/common/ql_expr.h"
#include "yb/docdb/doc_operation.h"
#include "yb/docdb/docdb.h"
#include "yb/docdb/docdb_util.h"
#include "yb/docdb/docdb_rocksdb_util.h"
#include "yb/docdb/doc_expr.h"
#include "yb/docdb/doc_rowwise_iterator.h"
#include "yb/docdb/doc_ql_scanspec.h"
#include "yb/docdb/subdocument.h"
#include "yb/server/hybrid_clock.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/util/trace.h"

DECLARE_bool(trace_docdb_calls);

using strings::Substitute;
using yb::bfql::TSOpcode;

DEFINE_bool(emulate_redis_responses,
    true,
    "If emulate_redis_responses is false, we hope to get slightly better performance by just "
    "returning OK for commands that might require us to read additional records viz. SADD, HSET, "
    "and HDEL. If emulate_redis_responses is true, we read the required records to compute the "
    "response as specified by the official Redis API documentation. https://redis.io/commands");

namespace yb {
namespace docdb {

using std::set;
using std::list;
using std::make_shared;
using strings::Substitute;

void RedisWriteOperation::GetDocPathsToLock(list<DocPath> *paths, IsolationLevel *level) const {
  paths->push_back(DocPath::DocPathFromRedisKey(request_.key_value().hash_code(),
                                                request_.key_value().key()));
  *level = IsolationLevel::SNAPSHOT_ISOLATION;
}

namespace {

bool EmulateRedisResponse(const RedisDataType& data_type) {
  return FLAGS_emulate_redis_responses && data_type != REDIS_TYPE_TIMESERIES;
}

CHECKED_STATUS PrimitiveValueFromSubKey(const RedisKeyValueSubKeyPB &subkey_pb,
                                        PrimitiveValue *primitive_value) {
  switch (subkey_pb.subkey_case()) {
    case RedisKeyValueSubKeyPB::SubkeyCase::kStringSubkey:
      *primitive_value = PrimitiveValue(subkey_pb.string_subkey());
      break;
    case RedisKeyValueSubKeyPB::SubkeyCase::kTimestampSubkey:
      // We use descending order for the timestamp in the timeseries type so that the latest
      // value sorts on top.
      *primitive_value = PrimitiveValue(subkey_pb.timestamp_subkey(), SortOrder::kDescending);
      break;
    case RedisKeyValueSubKeyPB::SubkeyCase::kDoubleSubkey: {
      *primitive_value = PrimitiveValue::Double(subkey_pb.double_subkey());
      break;
    }
    default:
      return STATUS_SUBSTITUTE(IllegalState, "Invalid enum value $0", subkey_pb.subkey_case());
  }
  return Status::OK();
}

// Stricter version of the above when we know the exact datatype to expect.
CHECKED_STATUS PrimitiveValueFromSubKeyStrict(const RedisKeyValueSubKeyPB &subkey_pb,
                                              const RedisDataType &data_type,
                                              PrimitiveValue *primitive_value) {
  switch (data_type) {
    case REDIS_TYPE_LIST: FALLTHROUGH_INTENDED;
    case REDIS_TYPE_SET: FALLTHROUGH_INTENDED;
    case REDIS_TYPE_HASH:
      if (!subkey_pb.has_string_subkey()) {
        return STATUS_SUBSTITUTE(InvalidArgument, "subkey: $0 should be of string type",
                                 subkey_pb.ShortDebugString());
      }
      break;
    case REDIS_TYPE_TIMESERIES:
      if (!subkey_pb.has_timestamp_subkey()) {
        return STATUS_SUBSTITUTE(InvalidArgument, "subkey: $0 should be of int64 type",
                                 subkey_pb.ShortDebugString());
      }
      break;
    case REDIS_TYPE_SORTEDSET:
      if (!subkey_pb.has_double_subkey()) {
        return STATUS_SUBSTITUTE(InvalidArgument, "subkey: $0 should be of double type",
                             subkey_pb.ShortDebugString());
      }
      break;
    default:
      return STATUS_SUBSTITUTE(IllegalState, "Invalid enum value $0", data_type);
  }
  return PrimitiveValueFromSubKey(subkey_pb, primitive_value);
}

Result<RedisDataType> GetRedisValueType(
    rocksdb::DB* rocksdb,
    const ReadHybridTime& read_time,
    const RedisKeyValuePB &key_value_pb,
    rocksdb::QueryId redis_query_id,
    DocWriteBatch* doc_write_batch = nullptr,
    int subkey_index = -1) {
  if (!key_value_pb.has_key()) {
    return STATUS(Corruption, "Expected KeyValuePB");
  }
  SubDocKey subdoc_key;
  if (subkey_index < 0) {
    subdoc_key = SubDocKey(DocKey::FromRedisKey(key_value_pb.hash_code(), key_value_pb.key()));
  } else {
    if (subkey_index >= key_value_pb.subkey_size()) {
      return STATUS_SUBSTITUTE(InvalidArgument,
                               "Size of subkeys ($0) must be larger than subkey_index ($1)",
                               key_value_pb.subkey_size(), subkey_index);
    }

    PrimitiveValue subkey_primitive;
    RETURN_NOT_OK(PrimitiveValueFromSubKey(key_value_pb.subkey(subkey_index), &subkey_primitive));
    subdoc_key = SubDocKey(DocKey::FromRedisKey(key_value_pb.hash_code(), key_value_pb.key()),
                           subkey_primitive);
  }
  SubDocument doc;
  bool doc_found = false;

  // Use the cached entry if possible to determine the value type.
  boost::optional<DocWriteBatchCache::Entry> cached_entry;
  if (doc_write_batch) {
    cached_entry = doc_write_batch->LookupCache(subdoc_key.Encode());
  }
  if (cached_entry) {
    doc_found = true;
    doc = SubDocument(cached_entry->value_type);
  } else {
    // TODO(dtxn) - pass correct transaction context when we implement cross-shard transactions
    // support for Redis.
    GetSubDocumentData data = { &subdoc_key, &doc, &doc_found };
    data.return_type_only = true;
    RETURN_NOT_OK(GetSubDocument(
        rocksdb, data, redis_query_id, boost::none /* txn_op_context */, read_time));
  }

  if (!doc_found) {
    return REDIS_TYPE_NONE;
  }

  switch (doc.value_type()) {
    case ValueType::kInvalidValueType: FALLTHROUGH_INTENDED;
    case ValueType::kTombstone:
      return REDIS_TYPE_NONE;
    case ValueType::kObject:
      return REDIS_TYPE_HASH;
    case ValueType::kRedisSet:
      return REDIS_TYPE_SET;
    case ValueType::kRedisTS:
      return REDIS_TYPE_TIMESERIES;
    case ValueType::kRedisSortedSet:
      return REDIS_TYPE_SORTEDSET;
    case ValueType::kNull: FALLTHROUGH_INTENDED; // This value is a set member.
    case ValueType::kString:
      return REDIS_TYPE_STRING;
    default:
      return STATUS_FORMAT(Corruption,
                           "Unknown value type for redis record: $0",
                           static_cast<char>(doc.value_type()));
  }
}

Result<RedisValue> GetRedisValue(
    rocksdb::DB *rocksdb,
    const ReadHybridTime& read_time,
    const RedisKeyValuePB &key_value_pb,
    rocksdb::QueryId redis_query_id,
    int subkey_index = -1) {
  if (!key_value_pb.has_key()) {
    return STATUS(Corruption, "Expected KeyValuePB");
  }
  SubDocKey doc_key(DocKey::FromRedisKey(key_value_pb.hash_code(), key_value_pb.key()));

  if (!key_value_pb.subkey().empty()) {
    if (key_value_pb.subkey().size() != 1 && subkey_index == -1) {
      return STATUS_SUBSTITUTE(Corruption,
                               "Expected at most one subkey, got $0", key_value_pb.subkey().size());
    }
    PrimitiveValue subkey_primitive;
    RETURN_NOT_OK(PrimitiveValueFromSubKey(
        key_value_pb.subkey(subkey_index == -1 ? 0 : subkey_index),
        &subkey_primitive));
    doc_key.AppendSubKeysAndMaybeHybridTime(subkey_primitive);
  }

  SubDocument doc;
  bool doc_found = false;

  // TODO(dtxn) - pass correct transaction context when we implement cross-shard transactions
  // support for Redis.
  GetSubDocumentData data = { &doc_key, &doc, &doc_found };
  RETURN_NOT_OK(GetSubDocument(
      rocksdb, data, redis_query_id, boost::none /* txn_op_context */, read_time));

  if (!doc_found) {
    return RedisValue{REDIS_TYPE_NONE};
  }

  if (!doc.IsPrimitive()) {
    switch (doc.value_type()) {
      case ValueType::kObject:
        return RedisValue{REDIS_TYPE_HASH};
      case ValueType::kRedisTS:
        return RedisValue{REDIS_TYPE_TIMESERIES};
      case ValueType::kRedisSortedSet:
        return RedisValue{REDIS_TYPE_SORTEDSET};
      case ValueType::kRedisSet:
        return RedisValue{REDIS_TYPE_SET};
      default:
        return STATUS_SUBSTITUTE(IllegalState, "Invalid value type: $0",
                                 static_cast<int>(doc.value_type()));
    }
  }

  return RedisValue{REDIS_TYPE_STRING, doc.GetString()};
}

YB_STRONGLY_TYPED_BOOL(VerifySuccessIfMissing);

// Set response based on the type match. Return whether the type matches what's expected.
bool VerifyTypeAndSetCode(
    const RedisDataType expected_type,
    const RedisDataType actual_type,
    RedisResponsePB *response,
    VerifySuccessIfMissing verify_success_if_missing = VerifySuccessIfMissing::kFalse) {
  if (actual_type == RedisDataType::REDIS_TYPE_NONE) {
    if (verify_success_if_missing) {
      response->set_code(RedisResponsePB_RedisStatusCode_OK);
    } else {
      response->set_code(RedisResponsePB_RedisStatusCode_NOT_FOUND);
    }
    return verify_success_if_missing;
  }
  if (actual_type != expected_type) {
    response->set_code(RedisResponsePB_RedisStatusCode_WRONG_TYPE);
    return false;
  }
  response->set_code(RedisResponsePB_RedisStatusCode_OK);
  return true;
}

bool VerifyTypeAndSetCode(
    const docdb::ValueType expected_type,
    const docdb::ValueType actual_type,
    RedisResponsePB *response) {
  if (actual_type != expected_type) {
    response->set_code(RedisResponsePB_RedisStatusCode_WRONG_TYPE);
    return false;
  }
  response->set_code(RedisResponsePB_RedisStatusCode_OK);
  return true;
}

CHECKED_STATUS AddPrimitiveValueToResponseArray(const PrimitiveValue& value,
                                                RedisArrayPB* redis_array) {
  switch (value.value_type()) {
    case ValueType::kString: FALLTHROUGH_INTENDED;
    case ValueType::kStringDescending: {
      redis_array->add_elements(value.GetString());
      return Status::OK();
    }
    case ValueType::kInt64: FALLTHROUGH_INTENDED;
    case ValueType::kInt64Descending: {
      redis_array->add_elements(std::to_string(value.GetInt64()));
      return Status::OK();
    }
    case ValueType::kDouble: FALLTHROUGH_INTENDED;
    case ValueType::kDoubleDescending: {
      redis_array->add_elements(std::to_string(value.GetDouble()));
      return Status::OK();
    }
    default:
      return STATUS_SUBSTITUTE(InvalidArgument, "Invalid value type: $0",
                             static_cast<int>(value.value_type()));
  }
}

CHECKED_STATUS CheckUserTimestampForCollections(const UserTimeMicros user_timestamp) {
  if (user_timestamp != Value::kInvalidUserTimestamp) {
    return STATUS(InvalidArgument, "User supplied timestamp is only allowed for "
        "replacing the whole collection");
  }
  return Status::OK();
}


CHECKED_STATUS AddResponseValuesGeneric(const PrimitiveValue& first,
                                        const PrimitiveValue& second,
                                        RedisResponsePB* response,
                                        bool add_keys,
                                        bool add_values) {
  if (add_keys) {
    RETURN_NOT_OK(AddPrimitiveValueToResponseArray(first, response->mutable_array_response()));
  }
  if (add_values) {
    RETURN_NOT_OK(AddPrimitiveValueToResponseArray(second, response->mutable_array_response()));
  }
  return Status::OK();
}

CHECKED_STATUS AddResponseValuesSortedSets(const PrimitiveValue& first,
                                           const SubDocument& second,
                                           RedisResponsePB* response,
                                           bool add_keys,
                                           bool add_values) {
  for (const auto& kv : second.object_container()) {
    const PrimitiveValue& value = kv.first;
    RETURN_NOT_OK(AddResponseValuesGeneric(first, value,
                                           response, add_keys, add_values));
  }
  return Status::OK();
}

template <typename T, typename AddResponseRow>
CHECKED_STATUS PopulateRedisResponseFromInternal(T iter,
                                                 AddResponseRow add_response_row,
                                                 const T& iter_end,
                                                 RedisResponsePB *response,
                                                 bool add_keys,
                                                 bool add_values) {
  response->set_allocated_array_response(new RedisArrayPB());
  for (; iter != iter_end; iter++) {
    RETURN_NOT_OK(add_response_row(iter->first, iter->second, response, add_keys, add_values));
  }
  return Status::OK();
}

template <typename AddResponseRow>
CHECKED_STATUS PopulateResponseFrom(const SubDocument::ObjectContainer &key_values,
                                    AddResponseRow add_response_row,
                                    RedisResponsePB *response,
                                    bool add_keys,
                                    bool add_values,
                                    bool reverse = false) {
  if (reverse) {
    return PopulateRedisResponseFromInternal(key_values.rbegin(), add_response_row,
                                             key_values.rend(), response, add_keys, add_values);
  } else {
    return PopulateRedisResponseFromInternal(key_values.begin(),  add_response_row,
                                             key_values.end(), response, add_keys, add_values);
  }
}

void SetOptionalInt(RedisDataType type, int64_t value, int64_t none_value,
                    RedisResponsePB* response) {
  response->set_int_response(type == RedisDataType::REDIS_TYPE_NONE ? none_value : value);
}

void SetOptionalInt(RedisDataType type, int64_t value, RedisResponsePB* response) {
  SetOptionalInt(type, value, 0, response);
}

CHECKED_STATUS GetCardinality(rocksdb::DB *rocksdb,
                              rocksdb::QueryId query_id,
                              ReadHybridTime hybrid_time,
                              const RedisKeyValuePB& kv,
                              int64_t *result) {
  SubDocKey key_card = SubDocKey(DocKey::FromRedisKey(kv.hash_code(), kv.key()),
                                    PrimitiveValue(ValueType::kCounter));
  SubDocument subdoc_card;

  bool subdoc_card_found = false;
  GetSubDocumentData data = { &key_card, &subdoc_card, &subdoc_card_found };
  RETURN_NOT_OK(GetSubDocument(
  rocksdb, data, query_id, boost::none /* txn_op_context */, hybrid_time));
  if (subdoc_card_found) {
    *result = subdoc_card.GetInt64();
  } else {
    *result = 0;
  }
  return Status::OK();
}

template <typename AddResponseValues>
CHECKED_STATUS GetAndPopulateResponseValues(
    rocksdb::DB* rocksdb,
    rocksdb::QueryId query_id,
    ReadHybridTime hybrid_time,
    AddResponseValues add_response_values,
    const SubDocKey& doc_key,
    ValueType expected_type,
    const SubDocKeyBound& low_subkey,
    const SubDocKeyBound& high_subkey,
    const RedisReadRequestPB& request,
    RedisResponsePB* response,
    bool add_keys, bool add_values, bool reverse) {

  SubDocument doc;
  bool doc_found = false;
  GetSubDocumentData data = { &doc_key, &doc, &doc_found };
  data.low_subkey = &low_subkey;
  data.high_subkey = &high_subkey;
  RETURN_NOT_OK(GetSubDocument(
  rocksdb, data, query_id, boost::none /* txn_op_context */, hybrid_time));

  // Validate and populate response.
  response->set_allocated_array_response(new RedisArrayPB());
  if (!doc_found) {
    response->set_code(RedisResponsePB_RedisStatusCode_OK);
    return Status::OK();
  }

  if (VerifyTypeAndSetCode(expected_type, doc.value_type(), response)) {
    RETURN_NOT_OK(PopulateResponseFrom(doc.object_container(),
                                       add_response_values,
                                       response, add_keys, add_values, reverse));
  }
  return Status::OK();
}

} // anonymous namespace

Status RedisWriteOperation::Apply(const DocOperationApplyData& data) {
  switch (request_.request_case()) {
    case RedisWriteRequestPB::RequestCase::kSetRequest:
      return ApplySet(data);
    case RedisWriteRequestPB::RequestCase::kGetsetRequest:
      return ApplyGetSet(data);
    case RedisWriteRequestPB::RequestCase::kAppendRequest:
      return ApplyAppend(data);
    case RedisWriteRequestPB::RequestCase::kDelRequest:
      return ApplyDel(data);
    case RedisWriteRequestPB::RequestCase::kSetRangeRequest:
      return ApplySetRange(data);
    case RedisWriteRequestPB::RequestCase::kIncrRequest:
      return ApplyIncr(data);
    case RedisWriteRequestPB::RequestCase::kPushRequest:
      return ApplyPush(data);
    case RedisWriteRequestPB::RequestCase::kInsertRequest:
      return ApplyInsert(data);
    case RedisWriteRequestPB::RequestCase::kPopRequest:
      return ApplyPop(data);
    case RedisWriteRequestPB::RequestCase::kAddRequest:
      return ApplyAdd(data);
    case RedisWriteRequestPB::RequestCase::REQUEST_NOT_SET: break;
  }
  return STATUS(Corruption,
      Substitute("Unsupported redis read operation: $0", request_.request_case()));
}

Result<RedisDataType> RedisWriteOperation::GetValueType(
    const DocOperationApplyData& data, int subkey_index) {
  return GetRedisValueType(
      data.doc_write_batch->rocksdb(), data.read_time, request_.key_value(),
      redis_query_id(), data.doc_write_batch, subkey_index);
}

Result<RedisValue> RedisWriteOperation::GetValue(
    const DocOperationApplyData& data, int subkey_index) {
  return GetRedisValue(data.doc_write_batch->rocksdb(), data.read_time,
                       request_.key_value(), redis_query_id(), subkey_index);
}

Status RedisWriteOperation::ApplySet(const DocOperationApplyData& data) {
  const RedisKeyValuePB& kv = request_.key_value();
  const MonoDelta ttl = request_.set_request().has_ttl() ?
      MonoDelta::FromMilliseconds(request_.set_request().ttl()) : Value::kMaxTtl;
  DocPath doc_path = DocPath::DocPathFromRedisKey(kv.hash_code(), kv.key());
  if (kv.subkey_size() > 0) {
    auto data_type = GetValueType(data);
    RETURN_NOT_OK(data_type);
    switch (kv.type()) {
      case REDIS_TYPE_TIMESERIES: FALLTHROUGH_INTENDED;
      case REDIS_TYPE_HASH: {
        if (*data_type != kv.type() && *data_type != REDIS_TYPE_NONE) {
          response_.set_code(RedisResponsePB_RedisStatusCode_WRONG_TYPE);
          return Status::OK();
        }
        SubDocument kv_entries = SubDocument();
        for (int i = 0; i < kv.subkey_size(); i++) {
          PrimitiveValue subkey_value;
          RETURN_NOT_OK(PrimitiveValueFromSubKeyStrict(kv.subkey(i), kv.type(), &subkey_value));
          kv_entries.SetChild(subkey_value,
                              SubDocument(PrimitiveValue(kv.value(i))));
        }

        if (kv.type() == REDIS_TYPE_TIMESERIES) {
          RETURN_NOT_OK(kv_entries.ConvertToRedisTS());
        }

        // For an HSET command (which has only one subkey), we need to read the subkey to find out
        // if the key already existed, and return 0 or 1 accordingly. This read is unnecessary for
        // HMSET and TSADD.
        if (kv.subkey_size() == 1 && EmulateRedisResponse(kv.type()) &&
            !request_.set_request().expect_ok_response()) {
          auto type = GetValueType(data, 0);
          RETURN_NOT_OK(type);
          // For HSET/TSADD, we return 0 or 1 depending on if the key already existed.
          // If flag is false, no int response is returned.
          SetOptionalInt(*type, 0, 1, &response_);
        }
        if (*data_type == REDIS_TYPE_NONE && kv.type() == REDIS_TYPE_TIMESERIES) {
          // Need to insert the document instead of extending it.
          RETURN_NOT_OK(data.doc_write_batch->InsertSubDocument(
              doc_path, kv_entries, redis_query_id(), ttl));
        } else {
          RETURN_NOT_OK(data.doc_write_batch->ExtendSubDocument(
              doc_path, kv_entries, redis_query_id(), ttl));
        }
        break;
      }
      case REDIS_TYPE_SORTEDSET: {
        if (*data_type != kv.type() && *data_type != REDIS_TYPE_NONE) {
          response_.set_code(RedisResponsePB_RedisStatusCode_WRONG_TYPE);
          return Status::OK();
        }

        // The SubDocuments to be inserted for card, the forward mapping, and reverse mapping.
        SubDocument kv_entries_card;
        SubDocument kv_entries_forward;
        SubDocument kv_entries_reverse;

        // The top level mapping.
        SubDocument kv_entries;

        int new_elements_added = 0;
        int return_value = 0;
        for (int i = 0; i < kv.subkey_size(); i++) {
          // Check whether the value is already in the document, if so delete it.
          SubDocKey key_reverse = SubDocKey(DocKey::FromRedisKey(kv.hash_code(), kv.key()),
                                            PrimitiveValue(ValueType::kSSReverse),
                                            PrimitiveValue(kv.value(i)));
          SubDocument subdoc_reverse;
          bool subdoc_reverse_found = false;
          GetSubDocumentData get_data = { &key_reverse, &subdoc_reverse, &subdoc_reverse_found };
          RETURN_NOT_OK(GetSubDocument(
              data.doc_write_batch->rocksdb(), get_data, redis_query_id(),
              boost::none /* txn_op_context */, data.read_time));

          // Flag indicating whether we should add the given entry to the sorted set.
          bool should_add_entry = true;
          // Flag indicating whether we shoould remove an entry from the sorted set.
          bool should_remove_existing_entry = false;

          if (!subdoc_reverse_found) {
            // The value is not already in the document.
            switch (request_.set_request().sorted_set_options().update_options()) {
              case SortedSetOptionsPB_UpdateOptions_NX: FALLTHROUGH_INTENDED;
              case SortedSetOptionsPB_UpdateOptions_NONE: {
                // Both these options call for inserting new elements, increment return_value and
                // keep should_add_entry as true.
                return_value++;
                new_elements_added++;
                break;
              }
              default: {
                // XX option calls for no new elements, don't increment return_value and set
                // should_add_entry to false.
                should_add_entry = false;
                break;
              }
            }
          } else {
            // The value is already in the document.
            switch (request_.set_request().sorted_set_options().update_options()) {
              case SortedSetOptionsPB_UpdateOptions_XX:
              case SortedSetOptionsPB_UpdateOptions_NONE: {
                // First make sure that the new score is different from the old score.
                // Both these options call for updating existing elements, set
                // should_remove_existing_entry to true, and if the CH flag is on (return both
                // elements changed and elements added), increment return_value.
                double score_to_remove = subdoc_reverse.GetDouble();
                if (score_to_remove != kv.subkey(i).double_subkey()) {
                  should_remove_existing_entry = true;
                  if (request_.set_request().sorted_set_options().ch()) {
                    return_value++;
                  }
                }
                break;
              }
              default: {
                // NX option calls for only new elements, set should_add_entry to false.
                should_add_entry = false;
                break;
              }
            }
          }

          if (should_remove_existing_entry) {
            double score_to_remove = subdoc_reverse.GetDouble();
            SubDocument subdoc_forward_tombstone;
            subdoc_forward_tombstone.SetChild(PrimitiveValue(kv.value(i)),
                                              SubDocument(ValueType::kTombstone));
            kv_entries_forward.SetChild(PrimitiveValue::Double(score_to_remove),
                                        SubDocument(subdoc_forward_tombstone));
          }

          if (should_add_entry) {
            // If the incr option is specified, we need insert the existing score + new score
            // instead of just the new score.
            double score_to_add = request_.set_request().sorted_set_options().incr() ?
                kv.subkey(i).double_subkey() + subdoc_reverse.GetDouble() :
                kv.subkey(i).double_subkey();

            // Add the forward mapping to the entries.
            SubDocument *forward_entry =
                kv_entries_forward.GetOrAddChild(PrimitiveValue::Double(score_to_add)).first;
            forward_entry->SetChild(PrimitiveValue(kv.value(i)),
                                    SubDocument(PrimitiveValue()));

            // Add the reverse mapping to the entries.
            kv_entries_reverse.SetChild(PrimitiveValue(kv.value(i)),
                                        SubDocument(PrimitiveValue::Double(score_to_add)));
          }
        }

        if (new_elements_added > 0) {
          int64_t card;
          RETURN_NOT_OK(GetCardinality(data.doc_write_batch->rocksdb(), redis_query_id(),
                                       data.read_time, kv, &card));
          // Insert card + new_elements_added back into the document for the updated card.
          kv_entries_card = SubDocument(PrimitiveValue(card + new_elements_added));
          kv_entries.SetChild(PrimitiveValue(ValueType::kCounter), SubDocument(kv_entries_card));
        }

        if (kv_entries_forward.object_num_keys() > 0) {
          kv_entries.SetChild(PrimitiveValue(ValueType::kSSForward),
                              SubDocument(kv_entries_forward));
        }

        if (kv_entries_reverse.object_num_keys() > 0) {
          kv_entries.SetChild(PrimitiveValue(ValueType::kSSReverse),
                              SubDocument(kv_entries_reverse));
        }

        if (kv_entries.object_num_keys() > 0) {
          RETURN_NOT_OK(kv_entries.ConvertToRedisSortedSet());
          if (*data_type == REDIS_TYPE_NONE) {
                RETURN_NOT_OK(data.doc_write_batch->InsertSubDocument(
                doc_path, kv_entries, redis_query_id(), ttl));
          } else {
                RETURN_NOT_OK(data.doc_write_batch->ExtendSubDocument(
                doc_path, kv_entries, redis_query_id(), ttl));
          }
        }
        response_.set_int_response(return_value);
        break;
      }
      case REDIS_TYPE_STRING: {
        return STATUS_SUBSTITUTE(InvalidCommand,
            "Redis data type $0 in SET command should not have subkeys", kv.type());
      }
      default:
        return STATUS_SUBSTITUTE(InvalidCommand,
            "Redis data type $0 not supported in SET command", kv.type());
    }
  } else {
    if (kv.type() != REDIS_TYPE_STRING) {
      return STATUS_SUBSTITUTE(InvalidCommand,
          "Redis data type for SET must be string if subkey not present, found $0", kv.type());
    }
    if (kv.value_size() != 1) {
      return STATUS_SUBSTITUTE(InvalidCommand,
          "There must be only one value in SET if there is only one key, found $0",
          kv.value_size());
    }
    const RedisWriteMode mode = request_.set_request().mode();
    if (mode != RedisWriteMode::REDIS_WRITEMODE_UPSERT) {
      auto data_type = GetValueType(data);
      RETURN_NOT_OK(data_type);
      if ((mode == RedisWriteMode::REDIS_WRITEMODE_INSERT && *data_type != REDIS_TYPE_NONE)
          || (mode == RedisWriteMode::REDIS_WRITEMODE_UPDATE && *data_type == REDIS_TYPE_NONE)) {
        response_.set_code(RedisResponsePB_RedisStatusCode_NOT_FOUND);
        return Status::OK();
      }
    }
    RETURN_NOT_OK(data.doc_write_batch->SetPrimitive(
        doc_path, Value(PrimitiveValue(kv.value(0)), ttl),
        redis_query_id()));
  }
  response_.set_code(RedisResponsePB_RedisStatusCode_OK);
  return Status::OK();
}

Status RedisWriteOperation::ApplyGetSet(const DocOperationApplyData& data) {
  const RedisKeyValuePB& kv = request_.key_value();

  auto value = GetValue(data);
  RETURN_NOT_OK(value);

  if (kv.value_size() != 1) {
    return STATUS_SUBSTITUTE(Corruption,
        "Getset kv should have 1 value, found $0", kv.value_size());
  }

  if (!VerifyTypeAndSetCode(RedisDataType::REDIS_TYPE_STRING, value->type, &response_)) {
    // We've already set the error code in the response.
    return Status::OK();
  }
  response_.set_string_response(value->value);

  return data.doc_write_batch->SetPrimitive(
      DocPath::DocPathFromRedisKey(kv.hash_code(), kv.key()),
      Value(PrimitiveValue(kv.value(0))), redis_query_id());
}

Status RedisWriteOperation::ApplyAppend(const DocOperationApplyData& data) {
  const RedisKeyValuePB& kv = request_.key_value();

  if (kv.value_size() != 1) {
    return STATUS_SUBSTITUTE(Corruption,
        "Append kv should have 1 value, found $0", kv.value_size());
  }

  auto value = GetValue(data);
  RETURN_NOT_OK(value);

  if (!VerifyTypeAndSetCode(RedisDataType::REDIS_TYPE_STRING, value->type, &response_,
                            VerifySuccessIfMissing::kTrue)) {
    // We've already set the error code in the response.
    return Status::OK();
  }

  value->value += kv.value(0);

  response_.set_int_response(value->value.length());

  return data.doc_write_batch->SetPrimitive(
      DocPath::DocPathFromRedisKey(kv.hash_code(), kv.key()), Value(PrimitiveValue(value->value)),
      redis_query_id());
}

// TODO (akashnil): Actually check if the value existed, return 0 if not. handle multidel in future.
//                  See ENG-807
Status RedisWriteOperation::ApplyDel(const DocOperationApplyData& data) {
  const RedisKeyValuePB& kv = request_.key_value();
  auto data_type = GetValueType(data);
  RETURN_NOT_OK(data_type);
  if (*data_type != REDIS_TYPE_NONE && *data_type != kv.type() && kv.type() != REDIS_TYPE_NONE) {
    response_.set_code(RedisResponsePB_RedisStatusCode_WRONG_TYPE);
    return Status::OK();
  }

  SubDocument values;
  // Number of distinct keys being removed.
  int num_keys = 0;
  switch (kv.type()) {
    case REDIS_TYPE_NONE: {
      values = SubDocument(ValueType::kTombstone);
      num_keys = *data_type == REDIS_TYPE_NONE ? 0 : 1;
      break;
    }
    case REDIS_TYPE_TIMESERIES: {
      if (*data_type == REDIS_TYPE_NONE) {
        return Status::OK();
      }
      for (int i = 0; i < kv.subkey_size(); i++) {
        PrimitiveValue primitive_value;
        RETURN_NOT_OK(PrimitiveValueFromSubKeyStrict(kv.subkey(i), *data_type, &primitive_value));
        values.SetChild(primitive_value, SubDocument(ValueType::kTombstone));
      }
      break;
    }
    case REDIS_TYPE_SORTEDSET: {
      SubDocument values_card;
      SubDocument values_forward;
      SubDocument values_reverse;
      num_keys = kv.subkey_size();
      for (int i = 0; i < kv.subkey_size(); i++) {
        // Check whether the value is already in the document.
        SubDocument doc_reverse;
        bool doc_reverse_found = false;
        SubDocKey subdoc_key_reverse = SubDocKey(DocKey::FromRedisKey(kv.hash_code(), kv.key()),
                                                 PrimitiveValue(ValueType::kSSReverse),
                                                 PrimitiveValue(kv.subkey(i).string_subkey()));
        // Todo(Rahul): Add values to the write batch cache and then do an additional check.
        // As of now, we only check to see if a value is in rocksdb, and we should also check
        // the write batch.
        GetSubDocumentData get_data = { &subdoc_key_reverse, &doc_reverse, &doc_reverse_found };
        RETURN_NOT_OK(GetSubDocument(
        data.doc_write_batch->rocksdb(), get_data, redis_query_id(),
        boost::none /* txn_op_context */, data.read_time));
        if (doc_reverse_found && doc_reverse.value_type() != ValueType::kTombstone) {
          // The value is already in the doc, needs to be removed.
          values_reverse.SetChild(PrimitiveValue(kv.subkey(i).string_subkey()),
                          SubDocument(ValueType::kTombstone));
          // For sorted sets, the forward mapping also needs to be deleted.
          SubDocument doc_forward;
          doc_forward.SetChild(PrimitiveValue(kv.subkey(i).string_subkey()),
                               SubDocument(ValueType::kTombstone));
          values_forward.SetChild(PrimitiveValue::Double(doc_reverse.GetDouble()),
                          SubDocument(doc_forward));
        } else {
          // If the key is absent, it doesn't contribute to the count of keys being deleted.
          num_keys--;
        }
      }
      int64_t card;
      RETURN_NOT_OK(GetCardinality(data.doc_write_batch->rocksdb(), redis_query_id(),
                                   data.read_time, kv, &card));
      // The new cardinality is card - num_keys.
      values_card = SubDocument(PrimitiveValue(card - num_keys));

      values.SetChild(PrimitiveValue(ValueType::kCounter), SubDocument(values_card));
      values.SetChild(PrimitiveValue(ValueType::kSSForward), SubDocument(values_forward));
      values.SetChild(PrimitiveValue(ValueType::kSSReverse), SubDocument(values_reverse));

      break;
    }
    default: {
      num_keys = kv.subkey_size(); // We know the subkeys are distinct.
      // Avoid reads for redis timeseries type.
      if (EmulateRedisResponse(kv.type())) {
        for (int i = 0; i < kv.subkey_size(); i++) {
          auto type = GetValueType(data, i);
          RETURN_NOT_OK(type);
          if (*type == REDIS_TYPE_STRING) {
            values.SetChild(PrimitiveValue(kv.subkey(i).string_subkey()),
                            SubDocument(ValueType::kTombstone));
          } else {
            // If the key is absent, it doesn't contribute to the count of keys being deleted.
            num_keys--;
          }
        }
      }
      break;
    }
  }
  DocPath doc_path = DocPath::DocPathFromRedisKey(kv.hash_code(), kv.key());
  RETURN_NOT_OK(data.doc_write_batch->ExtendSubDocument(
      doc_path, values, redis_query_id()));
  response_.set_code(RedisResponsePB_RedisStatusCode_OK);
  if (EmulateRedisResponse(kv.type())) {
    // If the flag is true, we respond with the number of keys actually being deleted. We don't
    // report this number for the redis timeseries type to avoid reads.
    response_.set_int_response(num_keys);
  }
  return Status::OK();
}

Status RedisWriteOperation::ApplySetRange(const DocOperationApplyData& data) {
  const RedisKeyValuePB& kv = request_.key_value();
  if (kv.value_size() != 1) {
    return STATUS_SUBSTITUTE(Corruption,
        "SetRange kv should have 1 value, found $0", kv.value_size());
  }

  auto value = GetValue(data);
  RETURN_NOT_OK(value);

  if (!VerifyTypeAndSetCode(RedisDataType::REDIS_TYPE_STRING, value->type, &response_,
                            VerifySuccessIfMissing::kTrue)) {
    // We've already set the error code in the response.
    return Status::OK();
  }

  // TODO (akashnil): Handle overflows.
  if (request_.set_range_request().offset() > value->value.length()) {
    value->value.resize(request_.set_range_request().offset(), 0);
  }
  value->value.replace(request_.set_range_request().offset(), kv.value(0).length(), kv.value(0));
  response_.set_int_response(value->value.length());

  return data.doc_write_batch->SetPrimitive(
      DocPath::DocPathFromRedisKey(kv.hash_code(), kv.key()),
      Value(PrimitiveValue(value->value)), redis_query_id());
}

Status RedisWriteOperation::ApplyIncr(const DocOperationApplyData& data, int64_t incr) {
  const RedisKeyValuePB& kv = request_.key_value();

  auto value = GetValue(data);
  RETURN_NOT_OK(value);

  if (!VerifyTypeAndSetCode(RedisDataType::REDIS_TYPE_STRING, value->type, &response_)) {
    // We've already set the error code in the response.
    return Status::OK();
  }

  int64_t old_value, new_value;

  try {
    old_value = std::stoll(value->value);
    new_value = old_value + incr;
  } catch (std::invalid_argument e) {
    response_.set_error_message("Can not parse incr argument as a number");
    return Status::OK();
  } catch (std::out_of_range e) {
    response_.set_error_message("Can not parse incr argument as a number");
    return Status::OK();
  }

  if ((incr < 0 && old_value < 0 && incr < numeric_limits<int64_t>::min() - old_value) ||
      (incr > 0 && old_value > 0 && incr > numeric_limits<int64_t>::max() - old_value)) {
    response_.set_error_message("Increment would overflow");
    return Status::OK();
  }

  response_.set_int_response(new_value);

  return data.doc_write_batch->SetPrimitive(
      DocPath::DocPathFromRedisKey(kv.hash_code(), kv.key()),
      Value(PrimitiveValue(std::to_string(new_value))),
      redis_query_id());
}

Status RedisWriteOperation::ApplyPush(const DocOperationApplyData& data) {
  return STATUS(NotSupported, "Redis operation has not been implemented");
}

Status RedisWriteOperation::ApplyInsert(const DocOperationApplyData& data) {
  return STATUS(NotSupported, "Redis operation has not been implemented");
}

Status RedisWriteOperation::ApplyPop(const DocOperationApplyData& data) {
  return STATUS(NotSupported, "Redis operation has not been implemented");
}

Status RedisWriteOperation::ApplyAdd(const DocOperationApplyData& data) {
  const RedisKeyValuePB& kv = request_.key_value();
  auto data_type = GetValueType(data);
  RETURN_NOT_OK(data_type);

  if (*data_type != REDIS_TYPE_SET && *data_type != REDIS_TYPE_NONE) {
    response_.set_code(RedisResponsePB_RedisStatusCode_WRONG_TYPE);
    return Status::OK();
  }

  DocPath doc_path = DocPath::DocPathFromRedisKey(kv.hash_code(), kv.key());

  if (kv.subkey_size() == 0) {
    return STATUS(InvalidCommand, "SADD request has no subkeys set");
  }

  int num_keys_found = 0;

  SubDocument set_entries = SubDocument();

  for (int i = 0 ; i < kv.subkey_size(); i++) { // We know that each subkey is distinct.
    if (FLAGS_emulate_redis_responses) {
      auto type = GetValueType(data, i);
      RETURN_NOT_OK(type);
      if (*type != REDIS_TYPE_NONE) {
        num_keys_found++;
      }
    }

    set_entries.SetChild(
        PrimitiveValue(kv.subkey(i).string_subkey()),
        SubDocument(PrimitiveValue(ValueType::kNull)));
  }

  RETURN_NOT_OK(set_entries.ConvertToRedisSet());

  Status s;

  if (*data_type == REDIS_TYPE_NONE) {
    RETURN_NOT_OK(data.doc_write_batch->InsertSubDocument(doc_path, set_entries, redis_query_id()));
  } else {
    RETURN_NOT_OK(data.doc_write_batch->ExtendSubDocument(doc_path, set_entries, redis_query_id()));
  }

  response_.set_code(RedisResponsePB_RedisStatusCode_OK);
  if (FLAGS_emulate_redis_responses) {
    // If flag is set, the actual number of new keys added is sent as response.
    response_.set_int_response(kv.subkey_size() - num_keys_found);
  }
  return Status::OK();
}

Status RedisWriteOperation::ApplyRemove(const DocOperationApplyData& data) {
  return STATUS(NotSupported, "Redis operation has not been implemented");
}

Status RedisReadOperation::Execute() {
  switch (request_.request_case()) {
    case RedisReadRequestPB::RequestCase::kGetRequest:
      return ExecuteGet();
    case RedisReadRequestPB::RequestCase::kStrlenRequest:
      return ExecuteStrLen();
    case RedisReadRequestPB::RequestCase::kExistsRequest:
      return ExecuteExists();
    case RedisReadRequestPB::RequestCase::kGetRangeRequest:
      return ExecuteGetRange();
    case RedisReadRequestPB::RequestCase::kGetCollectionRangeRequest:
      return ExecuteCollectionGetRange();
    default:
      return STATUS(Corruption,
          Substitute("Unsupported redis write operation: $0", request_.request_case()));
  }
}

int RedisReadOperation::ApplyIndex(int32_t index, const int32_t len) {
  if (index < 0) index += len;
  if (index < 0 || index >= len)
    return -1;
  return index;
}

Status RedisReadOperation::ExecuteHGetAllLikeCommands(ValueType value_type,
                                                      bool add_keys,
                                                      bool add_values) {
  SubDocKey doc_key(
      DocKey::FromRedisKey(request_.key_value().hash_code(), request_.key_value().key()));
  SubDocument doc;
  bool doc_found = false;
  // TODO(dtxn) - pass correct transaction context when we implement cross-shard transactions
  // support for Redis.
  GetSubDocumentData data = { &doc_key, &doc, &doc_found };
  switch (value_type) {
    case ValueType::kRedisSortedSet: {
      if (add_keys || add_values) {
        RETURN_NOT_OK(GetSubDocument(
        db_, data, redis_query_id(), boost::none /* txn_op_context */, read_time_));
        response_.set_allocated_array_response(new RedisArrayPB());
        if (!doc_found) {
          response_.set_code(RedisResponsePB_RedisStatusCode_OK);
          return Status::OK();
        }
        if (VerifyTypeAndSetCode(value_type, doc.value_type(), &response_)) {
          RETURN_NOT_OK(PopulateResponseFrom(doc.object_container(), AddResponseValuesGeneric,
                                             &response_, add_keys, add_values));
        }
      } else {
        int64_t card;
        RETURN_NOT_OK(GetCardinality(db_, redis_query_id(), read_time_,
                                     request_.key_value(), &card));
        response_.set_int_response(card);
      }
      break;
    }
    default: {
      RETURN_NOT_OK(GetSubDocument(
      db_, data, redis_query_id(), boost::none /* txn_op_context */, read_time_));
      if (add_keys || add_values) {
        response_.set_allocated_array_response(new RedisArrayPB());
      }
      if (!doc_found) {
        response_.set_code(RedisResponsePB_RedisStatusCode_OK);
        return Status::OK();
      }
      if (VerifyTypeAndSetCode(value_type, doc.value_type(), &response_)) {
        if (add_keys || add_values) {
          RETURN_NOT_OK(PopulateResponseFrom(doc.object_container(), AddResponseValuesGeneric,
                                             &response_, add_keys, add_values));
        } else {
          response_.set_int_response(doc.object_container().size());
        }
      }
      break;
    }
  }
  return Status::OK();
}

Status RedisReadOperation::ExecuteCollectionGetRange() {
  const RedisKeyValuePB& key_value = request_.key_value();
  if (!request_.has_key_value() || !key_value.has_key() || !request_.has_subkey_range() ||
      !request_.subkey_range().has_lower_bound() || !request_.subkey_range().has_upper_bound()) {
    return STATUS(InvalidArgument, "Need to specify the key and the subkey range");
  }

  const auto request_type = request_.get_collection_range_request().request_type();
  switch (request_type) {
    case RedisCollectionGetRangeRequestPB_GetRangeRequestType_ZRANGEBYSCORE: FALLTHROUGH_INTENDED;
    case RedisCollectionGetRangeRequestPB_GetRangeRequestType_TSRANGEBYTIME: {
      const RedisSubKeyBoundPB& lower_bound = request_.subkey_range().lower_bound();
      const RedisSubKeyBoundPB& upper_bound = request_.subkey_range().upper_bound();

      if ((lower_bound.has_infinity_type() &&
          lower_bound.infinity_type() == RedisSubKeyBoundPB_InfinityType_POSITIVE) ||
          (upper_bound.has_infinity_type() &&
              upper_bound.infinity_type() == RedisSubKeyBoundPB_InfinityType_NEGATIVE)) {
        // Return empty response.
        response_.set_code(RedisResponsePB_RedisStatusCode_OK);
        RETURN_NOT_OK(PopulateResponseFrom(SubDocument::ObjectContainer(), AddResponseValuesGeneric,
                                           &response_, /* add_keys */ true, /* add_values */ true));
        return Status::OK();
      }

      if (request_type == RedisCollectionGetRangeRequestPB_GetRangeRequestType_ZRANGEBYSCORE) {
        SubDocKey doc_key(
            DocKey::FromRedisKey(request_.key_value().hash_code(), request_.key_value().key()),
            PrimitiveValue(ValueType::kSSForward));
        double low_double = lower_bound.subkey_bound().double_subkey();
        double high_double = upper_bound.subkey_bound().double_subkey();

        SubDocKey low_sub_key_bound = SubDocKey(doc_key.doc_key(),
                                                PrimitiveValue(ValueType::kSSForward),
                                                PrimitiveValue::Double(low_double));

        SubDocKey high_sub_key_bound = SubDocKey(doc_key.doc_key(),
                                                 PrimitiveValue(ValueType::kSSForward),
                                                 PrimitiveValue::Double(high_double));

        SubDocKeyBound low_subkey = (lower_bound.has_infinity_type()) ?
            SubDocKeyBound() : SubDocKeyBound(low_sub_key_bound,
                                              lower_bound.is_exclusive(),
                                              /* is_lower_bound */ true);
        SubDocKeyBound high_subkey = (upper_bound.has_infinity_type()) ?
            SubDocKeyBound() : SubDocKeyBound(high_sub_key_bound,
                                              upper_bound.is_exclusive(),
                                              /* is_lower_bound */ false);

        bool add_keys = request_.get_collection_range_request().with_scores();

        RETURN_NOT_OK(GetAndPopulateResponseValues(
            db_, redis_query_id(), read_time_, AddResponseValuesSortedSets, doc_key,
            ValueType::kObject,  low_subkey, high_subkey, request_, &response_,
            /* add_keys */ add_keys, /* add_values */ true, /* reverse */ false));

      } else {
        SubDocKey doc_key(
            DocKey::FromRedisKey(request_.key_value().hash_code(), request_.key_value().key()));
        int64_t low_timestamp = lower_bound.subkey_bound().timestamp_subkey();
        int64_t high_timestamp = upper_bound.subkey_bound().timestamp_subkey();
        // Need to switch the order since we store the timestamps in descending order.
        SubDocKeyBound low_subkey = (upper_bound.has_infinity_type()) ?
            SubDocKeyBound() : SubDocKeyBound(SubDocKey(doc_key.doc_key(),
                                                        PrimitiveValue(high_timestamp,
                                                                       SortOrder::kDescending)),
                                              upper_bound.is_exclusive(), /* is_lower_bound */
                                              true);
        SubDocKeyBound high_subkey = (lower_bound.has_infinity_type()) ?
            SubDocKeyBound() : SubDocKeyBound(SubDocKey(doc_key.doc_key(),
                                                        PrimitiveValue(low_timestamp,
                                                                       SortOrder::kDescending)),
                                              lower_bound.is_exclusive(), /* is_lower_bound */
                                              false);
        RETURN_NOT_OK(GetAndPopulateResponseValues(
            db_, redis_query_id(), read_time_, AddResponseValuesGeneric, doc_key,
            ValueType::kRedisTS,  low_subkey, high_subkey, request_, &response_,
            /* add_keys */ true, /* add_values */ true, /* reverse */ true));
      }
      break;
    }
    case RedisCollectionGetRangeRequestPB_GetRangeRequestType_UNKNOWN:
      return STATUS(InvalidCommand, "Unknown Collection Get Range Request not supported");
  }
  return Status::OK();
}

Result<RedisDataType> RedisReadOperation::GetValueType(int subkey_index) {
  return GetRedisValueType(db_, read_time_, request_.key_value(), redis_query_id(),
                           nullptr /* doc_write_batch */, subkey_index);

}

Result<RedisValue> RedisReadOperation::GetValue(int subkey_index) {
  return GetRedisValue(db_, read_time_, request_.key_value(), redis_query_id(), subkey_index);
}

Status RedisReadOperation::ExecuteGet() {
  const auto request_type = request_.get_request().request_type();
  switch (request_type) {
    case RedisGetRequestPB_GetRequestType_GET: FALLTHROUGH_INTENDED;
    case RedisGetRequestPB_GetRequestType_TSGET: FALLTHROUGH_INTENDED;
    case RedisGetRequestPB_GetRequestType_HGET: {
      auto value = GetValue();
      RETURN_NOT_OK(value);

      // If wrong type, we set the error code in the response.
      if (VerifyTypeAndSetCode(RedisDataType::REDIS_TYPE_STRING, value->type, &response_)) {
        response_.set_string_response(value->value);
      }
      return Status::OK();
    }
    case RedisGetRequestPB_GetRequestType_HEXISTS: FALLTHROUGH_INTENDED;
    case RedisGetRequestPB_GetRequestType_SISMEMBER: {
      auto type = GetValueType();
      RETURN_NOT_OK(type);
      auto expected_type = request_type == RedisGetRequestPB_GetRequestType_HEXISTS
              ? RedisDataType::REDIS_TYPE_HASH
              : RedisDataType::REDIS_TYPE_SET;
      if (VerifyTypeAndSetCode(expected_type, *type, &response_, VerifySuccessIfMissing::kTrue)) {
        auto subtype = GetValueType(0);
        RETURN_NOT_OK(subtype);
        SetOptionalInt(*subtype, 1, &response_);
      }
      return Status::OK();
    }
    case RedisGetRequestPB_GetRequestType_HSTRLEN: {
      auto type = GetValueType();
      RETURN_NOT_OK(type);
      if (VerifyTypeAndSetCode(RedisDataType::REDIS_TYPE_HASH, *type, &response_,
                               VerifySuccessIfMissing::kTrue)) {
        auto value = GetValue();
        RETURN_NOT_OK(value);
        SetOptionalInt(value->type, value->value.length(), &response_);
      }
      return Status::OK();
    }
    case RedisGetRequestPB_GetRequestType_MGET: {
      return STATUS(NotSupported, "MGET not yet supported");
    }
    case RedisGetRequestPB_GetRequestType_HMGET: {
      auto type = GetValueType();
      RETURN_NOT_OK(type);
      if (!VerifyTypeAndSetCode(RedisDataType::REDIS_TYPE_HASH, *type, &response_,
                                VerifySuccessIfMissing::kTrue)) {
        return Status::OK();
      }

      response_.set_allocated_array_response(new RedisArrayPB());
      for (int i = 0; i < request_.key_value().subkey_size(); i++) {
        // TODO: ENG-1803: It is inefficient to create a new iterator for each subkey causing a
        // new seek. Consider reusing the same iterator.
        auto value = GetValue(i);
        RETURN_NOT_OK(value);
        if (value->type == REDIS_TYPE_STRING) {
          response_.mutable_array_response()->add_elements(value->value);
        } else {
          response_.mutable_array_response()->add_elements(""); // Empty is nil response.
        }
      }
      response_.set_code(RedisResponsePB_RedisStatusCode_OK);
      return Status::OK();
    }
    case RedisGetRequestPB_GetRequestType_HGETALL:
      return ExecuteHGetAllLikeCommands(ValueType::kObject, true, true);
    case RedisGetRequestPB_GetRequestType_HKEYS:
      return ExecuteHGetAllLikeCommands(ValueType::kObject, true, false);
    case RedisGetRequestPB_GetRequestType_HVALS:
      return ExecuteHGetAllLikeCommands(ValueType::kObject, false, true);
    case RedisGetRequestPB_GetRequestType_HLEN:
      return ExecuteHGetAllLikeCommands(ValueType::kObject, false, false);
    case RedisGetRequestPB_GetRequestType_SMEMBERS:
      return ExecuteHGetAllLikeCommands(ValueType::kRedisSet, true, false);
    case RedisGetRequestPB_GetRequestType_SCARD:
      return ExecuteHGetAllLikeCommands(ValueType::kRedisSet, false, false);
    case RedisGetRequestPB_GetRequestType_ZCARD:
      return ExecuteHGetAllLikeCommands(ValueType::kRedisSortedSet, false, false);
    case RedisGetRequestPB_GetRequestType_UNKNOWN: {
      return STATUS(InvalidCommand, "Unknown Get Request not supported");
    }
  }
  return Status::OK();
}

Status RedisReadOperation::ExecuteStrLen() {
  auto value = GetValue();
  RETURN_NOT_OK(value);

  if (VerifyTypeAndSetCode(RedisDataType::REDIS_TYPE_STRING, value->type, &response_,
                           VerifySuccessIfMissing::kTrue)) {
    SetOptionalInt(value->type, value->value.length(), &response_);
  }

  return Status::OK();
}

Status RedisReadOperation::ExecuteExists() {
  auto value = GetValue();
  RETURN_NOT_OK(value);

  // We only support exist command with one argument currently.
  response_.set_code(RedisResponsePB_RedisStatusCode_OK);
  SetOptionalInt(value->type, 1, &response_);

  return Status::OK();
}

Status RedisReadOperation::ExecuteGetRange() {
  auto value = GetValue();
  RETURN_NOT_OK(value);

  if (!VerifyTypeAndSetCode(RedisDataType::REDIS_TYPE_STRING, value->type, &response_)) {
    // We've already set the error code in the response.
    return Status::OK();
  }

  const int32_t len = value->value.length();

  // We treat negative indices to refer backwards from the end of the string.
  const int32_t start = ApplyIndex(request_.get_range_request().start(), len);
  if (start == -1) {
    response_.set_code(RedisResponsePB_RedisStatusCode_INDEX_OUT_OF_BOUNDS);
    return Status::OK();
  }
  const int32_t end = ApplyIndex(request_.get_range_request().end(), len);
  if (end == -1 || end < start) {
    response_.set_code(RedisResponsePB_RedisStatusCode_INDEX_OUT_OF_BOUNDS);
    return Status::OK();
  }

  response_.set_string_response(value->value.c_str() + start, end - start + 1);
  return Status::OK();
}

const RedisResponsePB& RedisReadOperation::response() {
  return response_;
}

namespace {

bool RequireReadForExpressions(const QLWriteRequestPB& request) {
  // A QLWriteOperation requires a read if it contains an IF clause or an UPDATE assignment that
  // involves an expresion with a column reference. If the IF clause contains a condition that
  // involves a column reference, the column will be included in "column_refs". However, we cannot
  // rely on non-empty "column_ref" alone to decide if a read is required because "IF EXISTS" and
  // "IF NOT EXISTS" do not involve a column reference explicitly.
  return request.has_if_expr()
      || request.has_column_refs() && (!request.column_refs().ids().empty() ||
          !request.column_refs().static_ids().empty());
}

// If range key portion is missing and there are no targeted columns this is a range operation
// (e.g. range delete) -- it affects all rows within a hash key that match the where clause.
// Note: If target columns are given this could just be e.g. a delete targeting a static column
// which can also omit the range portion -- Analyzer will check these restrictions.
bool IsRangeOperation(const QLWriteRequestPB& request, const Schema& schema) {
  return schema.num_range_key_columns() > 0 &&
         request.range_column_values().empty() &&
         request.column_values().empty();
}

bool RequireRead(const QLWriteRequestPB& request, const Schema& schema) {
  // In case of a user supplied timestamp, we need a read (and hence appropriate locks for read
  // modify write) but it is at the docdb level on a per key basis instead of a QL read of the
  // latest row.
  bool has_user_timestamp = request.has_user_timestamp_usec();

  // We need to read the rows in the given range to find out which rows to write to.
  bool is_range_operation = IsRangeOperation(request, schema);

  return RequireReadForExpressions(request) || has_user_timestamp || is_range_operation;
}

// Append dummy entries in schema to table_row
// TODO(omer): this should most probably be added somewhere else
void AddProjection(const Schema& schema, QLTableRow* table_row) {
  for (size_t i = 0; i < schema.num_columns(); i++) {
    const auto& column_id = schema.column_id(i);
    table_row->AllocColumn(column_id);
  }
}

// Create projection schemas of static and non-static columns from a rowblock projection schema
// (for read) and a WHERE / IF condition (for read / write). "schema" is the full table schema
// and "rowblock_schema" is the selected columns from which we are splitting into static and
// non-static column portions.
CHECKED_STATUS CreateProjections(const Schema& schema, const QLReferencedColumnsPB& column_refs,
                                 Schema* static_projection, Schema* non_static_projection) {
  // The projection schemas are used to scan docdb. Keep the columns to fetch in sorted order for
  // more efficient scan in the iterator.
  set<ColumnId> static_columns, non_static_columns;

  // Add regular columns.
  for (int32_t id : column_refs.ids()) {
    const ColumnId column_id(id);
    if (!schema.is_key_column(column_id)) {
      non_static_columns.insert(column_id);
    }
  }

  // Add static columns.
  for (int32_t id : column_refs.static_ids()) {
    const ColumnId column_id(id);
    static_columns.insert(column_id);
  }

  RETURN_NOT_OK(
      schema.CreateProjectionByIdsIgnoreMissing(
          vector<ColumnId>(static_columns.begin(), static_columns.end()),
          static_projection));
  RETURN_NOT_OK(
      schema.CreateProjectionByIdsIgnoreMissing(
          vector<ColumnId>(non_static_columns.begin(), non_static_columns.end()),
          non_static_projection));

  return Status::OK();
}

CHECKED_STATUS PopulateRow(const QLTableRow& table_row,
                           const Schema& projection, size_t col_idx, QLRow* row) {
  for (size_t i = 0; i < projection.num_columns(); i++, col_idx++) {
    RETURN_NOT_OK(table_row.GetValue(projection.column_id(i), row->mutable_column(col_idx)));
  }
  return Status::OK();
}

// Outer join a static row with a non-static row.
// A join is successful if and only if for every hash key, the values in the static and the
// non-static row are either non-NULL and the same, or one of them is NULL. Therefore we say that
// a join is successful if the static row is empty, and in turn return true.
// Copies the entries from the static row into the non-static one.
bool JoinStaticRow(
    const Schema& schema, const Schema& static_projection, const QLTableRow& static_row,
    QLTableRow* non_static_row) {
  // The join is successful if the static row is empty
  if (static_row.IsEmpty()) {
    return true;
  }

  // Now we know that the static row is not empty. The non-static row cannot be empty, therefore
  // we know that both the static row and the non-static one have non-NULL entries for all
  // hash keys. Therefore if MatchColumn returns false, we know the join is unsuccessful.
  // TODO(neil)
  // - Need to assign TTL and WriteTime to their default values.
  // - Check if they should be compared and copied over. Most likely not needed as we don't allow
  //   selecting TTL and WriteTime for static columns.
  // - This copying function should be moved to QLTableRow class.
  for (size_t i = 0; i < schema.num_hash_key_columns(); i++) {
    if (!non_static_row->MatchColumn(schema.column_id(i), static_row)) {
      return false;
    }
  }

  // Join the static columns in the static row into the non-static row.
  for (size_t i = 0; i < static_projection.num_columns(); i++) {
    CHECK_OK(non_static_row->CopyColumn(static_projection.column_id(i), static_row));
  }

  return true;
}

// Join a non-static row with a static row.
// Returns true if the two rows match
bool JoinNonStaticRow(
    const Schema& schema, const Schema& static_projection, const QLTableRow& non_static_row,
    QLTableRow* static_row) {
  bool join_successful = true;

  for (size_t i = 0; i < schema.num_hash_key_columns(); i++) {
    if (!static_row->MatchColumn(schema.column_id(i), non_static_row)) {
      join_successful = false;
      break;
    }
  }

  if (!join_successful) {
    static_row->Clear();
    for (size_t i = 0; i < static_projection.num_columns(); i++) {
      static_row->AllocColumn(static_projection.column_id(i));
    }

    for (size_t i = 0; i < schema.num_hash_key_columns(); i++) {
      CHECK_OK(static_row->CopyColumn(schema.column_id(i), non_static_row));
    }
  }
  return join_successful;
}



} // namespace

Status QLWriteOperation::Init(QLWriteRequestPB* request, QLResponsePB* response) {
  response_ = response;
  require_read_ = RequireRead(*request, schema_);

  request_.Swap(request);
  // Determine if static / non-static columns are being written.
  bool write_static_columns = false;
  bool write_non_static_columns = false;
  for (const auto& column : request_.column_values()) {
    auto schema_column = schema_.column_by_id(ColumnId(column.column_id()));
    RETURN_NOT_OK(schema_column);
    if (schema_column->is_static()) {
      write_static_columns = true;
    } else {
      write_non_static_columns = true;
    }
    if (write_static_columns && write_non_static_columns) {
      break;
    }
  }

  bool is_range_operation = IsRangeOperation(request_, schema_);

  // We need the hashed key if writing to the static columns, and need primary key if writing to
  // non-static columns or writing the full primary key (i.e. range columns are present or table
  // does not have range columns).
  return InitializeKeys(
      write_static_columns || is_range_operation,
      write_non_static_columns || !request_.range_column_values().empty() ||
          schema_.num_range_key_columns() == 0);
}

Status QLWriteOperation::InitializeKeys(const bool hashed_key, const bool primary_key) {
  // Populate the hashed and range components in the same order as they are in the table schema.
  const auto& hashed_column_values = request_.hashed_column_values();
  const auto& range_column_values = request_.range_column_values();
  vector<PrimitiveValue> hashed_components;
  vector<PrimitiveValue> range_components;
  RETURN_NOT_OK(QLKeyColumnValuesToPrimitiveValues(
      hashed_column_values, schema_, 0,
      schema_.num_hash_key_columns(), &hashed_components));
  RETURN_NOT_OK(QLKeyColumnValuesToPrimitiveValues(
      range_column_values, schema_, schema_.num_hash_key_columns(),
      schema_.num_range_key_columns(), &range_components));

  // We need the hash key if writing to the static columns.
  if (hashed_key && hashed_doc_key_ == nullptr) {
    hashed_doc_key_.reset(new DocKey(request_.hash_code(), hashed_components));
    hashed_doc_path_.reset(new DocPath(hashed_doc_key_->Encode()));
  }
  // We need the primary key if writing to non-static columns or writing the full primary key
  // (i.e. range columns are present).
  if (primary_key && pk_doc_key_ == nullptr) {
    if (request_.has_hash_code() && !hashed_column_values.empty()) {
      pk_doc_key_.reset(new DocKey(request_.hash_code(), hashed_components, range_components));
    } else {
      // In case of syscatalog tables, we don't have any hash components.
      pk_doc_key_.reset(new DocKey(range_components));
    }
    pk_doc_path_.reset(new DocPath(pk_doc_key_->Encode()));
  }

  return Status::OK();
}

void QLWriteOperation::GetDocPathsToLock(list<DocPath> *paths, IsolationLevel *level) const {
  if (hashed_doc_path_ != nullptr)
    paths->push_back(*hashed_doc_path_);
  if (pk_doc_path_ != nullptr)
    paths->push_back(*pk_doc_path_);
  // When this write operation requires a read, it requires a read snapshot so paths will be locked
  // in snapshot isolation for consistency. Otherwise, pure writes will happen in serializable
  // isolation so that they will serialize but do not conflict with one another.
  //
  // Currently, only keys that are being written are locked, no lock is taken on read at the
  // snapshot isolation level.
  *level = require_read_ ? IsolationLevel::SNAPSHOT_ISOLATION
                         : IsolationLevel::SERIALIZABLE_ISOLATION;
}

Status QLWriteOperation::ReadColumns(const DocOperationApplyData& data,
                                     Schema *param_static_projection,
                                     Schema *param_non_static_projection,
                                     QLTableRow* table_row) {
  Schema *static_projection = param_static_projection;
  Schema *non_static_projection = param_non_static_projection;

  Schema local_static_projection;
  Schema local_non_static_projection;
  if (static_projection == nullptr) {
    static_projection = &local_static_projection;
  }
  if (non_static_projection == nullptr) {
    non_static_projection = &local_non_static_projection;
  }

  // Create projections to scan docdb.
  RETURN_NOT_OK(CreateProjections(schema_, request_.column_refs(),
                                  static_projection, non_static_projection));

  // Generate hashed / primary key depending on if static / non-static columns are referenced in
  // the if-condition.
  RETURN_NOT_OK(InitializeKeys(
      !static_projection->columns().empty(), !non_static_projection->columns().empty()));

  // Scan docdb for the static and non-static columns of the row using the hashed / primary key.
  if (hashed_doc_key_ != nullptr) {
    DocQLScanSpec spec(*static_projection, *hashed_doc_key_, request_.query_id());
    DocRowwiseIterator iterator(*static_projection, schema_, txn_op_context_,
                                data.doc_write_batch->rocksdb(), data.read_time);
    RETURN_NOT_OK(iterator.Init(spec));
    if (iterator.HasNext()) {
      RETURN_NOT_OK(iterator.NextRow(table_row));
    }
    data.restart_read_ht->MakeAtLeast(iterator.RestartReadHt());
  }
  if (pk_doc_key_ != nullptr) {
    DocQLScanSpec spec(*non_static_projection, *pk_doc_key_, request_.query_id());
    DocRowwiseIterator iterator(*non_static_projection, schema_, txn_op_context_,
                                data.doc_write_batch->rocksdb(), data.read_time);
    RETURN_NOT_OK(iterator.Init(spec));
    if (iterator.HasNext()) {
      RETURN_NOT_OK(iterator.NextRow(table_row));
    } else {
      // If no non-static column is found, the row does not exist and we should clear the static
      // columns in the map to indicate the row does not exist.
      table_row->Clear();
    }
    data.restart_read_ht->MakeAtLeast(iterator.RestartReadHt());
  }

  return Status::OK();
}

Status QLWriteOperation::IsConditionSatisfied(const QLConditionPB& condition,
                                              const DocOperationApplyData& data,
                                              bool* should_apply,
                                              std::unique_ptr<QLRowBlock>* rowblock,
                                              QLTableRow* table_row) {
  // Read column values.
  Schema static_projection, non_static_projection;
  RETURN_NOT_OK(ReadColumns(data, &static_projection, &non_static_projection, table_row));

  // See if the if-condition is satisfied.
  RETURN_NOT_OK(EvalCondition(condition, *table_row, should_apply));

  // Populate the result set to return the "applied" status, and optionally the present column
  // values if the condition is not satisfied and the row does exist (value_map is not empty).
  std::vector<ColumnSchema> columns;
  columns.emplace_back(ColumnSchema("[applied]", BOOL));
  if (!*should_apply && !table_row->IsEmpty()) {
    columns.insert(columns.end(),
                   static_projection.columns().begin(), static_projection.columns().end());
    columns.insert(columns.end(),
                   non_static_projection.columns().begin(), non_static_projection.columns().end());
  }
  rowblock->reset(new QLRowBlock(Schema(columns, 0)));
  QLRow& row = rowblock->get()->Extend();
  row.mutable_column(0)->set_bool_value(*should_apply);
  if (!*should_apply && !table_row->IsEmpty()) {
    RETURN_NOT_OK(PopulateRow(*table_row, static_projection, 1 /* begin col_idx */, &row));
    RETURN_NOT_OK(PopulateRow(*table_row, non_static_projection,
                              1 + static_projection.num_columns(), &row));
  }

  return Status::OK();
}

Status QLWriteOperation::Apply(const DocOperationApplyData& data) {
  bool should_apply = true;
  QLTableRow table_row;
  if (request_.has_if_expr()) {
    RETURN_NOT_OK(IsConditionSatisfied(request_.if_expr().condition(),
                                       data,
                                       &should_apply,
                                       &rowblock_,
                                       &table_row));
  } else if (RequireReadForExpressions(request_)) {
    RETURN_NOT_OK(ReadColumns(data, nullptr, nullptr, &table_row));
  }

  if (should_apply) {
    const MonoDelta ttl =
        request_.has_ttl() ? MonoDelta::FromMilliseconds(request_.ttl()) : Value::kMaxTtl;
    const UserTimeMicros user_timestamp = request_.has_user_timestamp_usec() ?
        request_.user_timestamp_usec() : Value::kInvalidUserTimestamp;

    switch (request_.type()) {
      // QL insert == update (upsert) to be consistent with Cassandra's semantics. In either
      // INSERT or UPDATE, if non-key columns are specified, they will be inserted which will cause
      // the primary key to be inserted also when necessary. Otherwise, we should insert the
      // primary key at least.
      case QLWriteRequestPB::QL_STMT_INSERT:
      case QLWriteRequestPB::QL_STMT_UPDATE: {
        // Add the appropriate liveness column only for inserts.
        // We never use init markers for QL to ensure we perform writes without any reads to
        // ensure our write path is fast while complicating the read path a bit.
        if (request_.type() == QLWriteRequestPB::QL_STMT_INSERT && pk_doc_path_ != nullptr) {
          const DocPath sub_path(pk_doc_path_->encoded_doc_key(),
                                 PrimitiveValue::SystemColumnId(SystemColumnIds::kLivenessColumn));
          const auto value = Value(PrimitiveValue(), ttl, user_timestamp);
          RETURN_NOT_OK(data.doc_write_batch->SetPrimitive(
              sub_path, value, request_.query_id()));
        }

        if (request_.column_values_size() <= 0) {
          break;
        }

        for (const auto& column_value : request_.column_values()) {
          if (!column_value.has_column_id()) {
            return STATUS_FORMAT(InvalidArgument, "column id missing: $0",
                                 column_value.DebugString());
          }
          const ColumnId column_id(column_value.column_id());
          const auto maybe_column = schema_.column_by_id(column_id);
          RETURN_NOT_OK(maybe_column);
          const ColumnSchema& column = *maybe_column;

          DocPath sub_path(column.is_static() ? hashed_doc_path_->encoded_doc_key()
                                              : pk_doc_path_->encoded_doc_key(),
                           PrimitiveValue(column_id));

          QLValue expr_result;
          RETURN_NOT_OK(EvalExpr(column_value.expr(), table_row, &expr_result));
          const TSOpcode write_instr = GetTSWriteInstruction(column_value.expr());
          const SubDocument& sub_doc =
              SubDocument::FromQLValuePB(expr_result.value(), column.sorting_type(), write_instr);

          // Typical case, setting a columns value
          if (column_value.subscript_args().empty()) {
            switch (write_instr) {
              case TSOpcode::kScalarInsert:
                RETURN_NOT_OK(data.doc_write_batch->InsertSubDocument(
                    sub_path, sub_doc, request_.query_id(), ttl, user_timestamp));
                break;
              case TSOpcode::kMapExtend:
              case TSOpcode::kSetExtend:
              case TSOpcode::kMapRemove:
              case TSOpcode::kSetRemove:
                RETURN_NOT_OK(CheckUserTimestampForCollections(user_timestamp));
                RETURN_NOT_OK(data.doc_write_batch->ExtendSubDocument(
                    sub_path, sub_doc, request_.query_id(), ttl));
                break;
              case TSOpcode::kListAppend:
                RETURN_NOT_OK(CheckUserTimestampForCollections(user_timestamp));
                RETURN_NOT_OK(data.doc_write_batch->ExtendList(
                    sub_path, sub_doc, ListExtendOrder::APPEND, request_.query_id(), ttl));
                break;
              case TSOpcode::kListPrepend:
                RETURN_NOT_OK(CheckUserTimestampForCollections(user_timestamp));
                RETURN_NOT_OK(data.doc_write_batch->ExtendList(
                    sub_path, sub_doc, ListExtendOrder::PREPEND, request_.query_id(), ttl));
                break;
              case TSOpcode::kListRemove:
                // TODO(akashnil or mihnea) this should call RemoveFromList once thats implemented
                // Currently list subtraction is computed in memory using builtin call so this
                // case should never be reached. Once it is implemented the corresponding case
                // from EvalQLExpressionPB should be uncommented to enable this optimization.
                RETURN_NOT_OK(CheckUserTimestampForCollections(user_timestamp));
                RETURN_NOT_OK(data.doc_write_batch->InsertSubDocument(
                    sub_path, sub_doc, request_.query_id(), ttl, user_timestamp));
                break;
              default:
                LOG(FATAL) << "Unsupported operation: " << static_cast<int>(write_instr);
                break;
            }
          } else {
            RETURN_NOT_OK(CheckUserTimestampForCollections(user_timestamp));

            // Setting the value for a sub-column
            // Currently we only support two cases here: `map['key'] = v` and `list[index] = v`)
            // Any other case should be rejected by the semantic analyser before getting here
            // Later when we support frozen or nested collections this code may need refactoring
            DCHECK_EQ(column_value.subscript_args().size(), 1);
            DCHECK(column_value.subscript_args(0).has_value()) << "An index must be a constant";
            switch (column.type()->main()) {
              case MAP: {
                const PrimitiveValue &pv = PrimitiveValue::FromQLValuePB(
                    column_value.subscript_args(0).value(),
                    ColumnSchema::SortingType::kNotSpecified);
                sub_path.AddSubKey(pv);
                RETURN_NOT_OK(data.doc_write_batch->InsertSubDocument(
                    sub_path, sub_doc, request_.query_id(), ttl, user_timestamp));
                break;
              }
              case LIST: {
                MonoDelta table_ttl = schema_.table_properties().HasDefaultTimeToLive() ?
                  MonoDelta::FromMilliseconds(schema_.table_properties().DefaultTimeToLive()) :
                  MonoDelta::kMax;

                // At YQL layer list indexes start at 0, but internally we start at 1.
                int index = column_value.subscript_args(0).value().int32_value() + 1;
                Status s = data.doc_write_batch->ReplaceInList(
                    sub_path, {index}, {sub_doc}, data.read_time.read, request_.query_id(),
                    table_ttl, ttl);

                // Don't crash tserver if this is index-out-of-bounds error
                if (s.IsQLError()) {
                  response_->set_status(QLResponsePB::YQL_STATUS_USAGE_ERROR);
                  response_->set_error_message(s.ToString());
                  return Status::OK();
                } else if (!s.ok()) {
                  return s;
                }

                break;
              }
              default: {
                LOG(ERROR) << "Unexpected type for setting subcolumn: "
                           << column.type()->ToString();
              }
            }
          }
        }
        break;
      }
      case QLWriteRequestPB::QL_STMT_DELETE: {
        // We have three cases:
        // 1. If non-key columns are specified, we delete only those columns.
        // 2. Otherwise, if range cols are missing, this must be a range delete.
        // 3. Otherwise, this is a normal delete.
        // Analyzer ensures these are the only cases before getting here (e.g. range deletes cannot
        // specify non-key columns).
        if (request_.column_values_size() > 0) {
          // Delete the referenced columns only.
          for (const auto& column_value : request_.column_values()) {
            CHECK(column_value.has_column_id())
                << "column id missing: " << column_value.DebugString();
            const ColumnId column_id(column_value.column_id());
            const auto column = schema_.column_by_id(column_id);
            RETURN_NOT_OK(column);
            const DocPath sub_path(
                column->is_static() ? hashed_doc_path_->encoded_doc_key()
                                    : pk_doc_path_->encoded_doc_key(),
                PrimitiveValue(column_id));
            RETURN_NOT_OK(data.doc_write_batch->DeleteSubDoc(sub_path,
                                                             request_.query_id(), user_timestamp));
          }
        } else if (IsRangeOperation(request_, schema_)) {
          // If the range columns are not specified, we read everything and delete all rows for
          // which the where condition matches.

          // Create the schema projection -- range deletes cannot reference non-primary key columns,
          // so the non-static projection is all we need, it should contain all referenced columns.
          Schema static_projection;
          Schema projection;
          RETURN_NOT_OK(CreateProjections(schema_, request_.column_refs(),
              &static_projection, &projection));

          // Construct the scan spec basing on the WHERE condition.
          vector<PrimitiveValue> hashed_components;
          RETURN_NOT_OK(QLKeyColumnValuesToPrimitiveValues(
              request_.hashed_column_values(), schema_, 0,
              schema_.num_hash_key_columns(), &hashed_components));

          DocQLScanSpec spec(projection, request_.hash_code(), -1, hashed_components,
              request_.has_where_expr() ? &request_.where_expr().condition() : nullptr,
              request_.query_id());

          // Create iterator.
          DocRowwiseIterator iterator(projection, schema_, txn_op_context_,
                                      data.doc_write_batch->rocksdb(), data.read_time);
          RETURN_NOT_OK(iterator.Init(spec));

          // Iterate through rows and delete those that match the condition.
          // TODO We do not lock here, so other write transactions coming in might appear partially
          // applied if they happen in the middle of a ranged delete.
          QLTableRow row;
          while (iterator.HasNext()) {
            row.Clear();
            RETURN_NOT_OK(iterator.NextRow(&row));

            // Match the row with the where condition before deleting it.
            bool match = false;
            RETURN_NOT_OK(spec.Match(row, &match));
            if (match) {
              DocKey row_key = iterator.row_key();
              DocPath row_path(row_key.Encode());
              RETURN_NOT_OK(DeleteRow(data.doc_write_batch, row_path));
            }
          }
          data.restart_read_ht->MakeAtLeast(iterator.RestartReadHt());
        } else {
          // Otherwise, delete the referenced row (all columns).
          RETURN_NOT_OK(DeleteRow(data.doc_write_batch, *pk_doc_path_));
        }
        break;
      }
    }
  }

  response_->set_status(QLResponsePB::YQL_STATUS_OK);

  return Status::OK();
}

Status QLWriteOperation::DeleteRow(DocWriteBatch* doc_write_batch,
                                   const DocPath row_path) {
  if (request_.has_user_timestamp_usec()) {
    // If user_timestamp is provided, we need to add a tombstone for each individual
    // column in the schema since we don't want to analyze this on the read path.
    for (int i = schema_.num_key_columns(); i < schema_.num_columns(); i++) {
      const DocPath sub_path(row_path.encoded_doc_key(),
                             PrimitiveValue(schema_.column_id(i)));
      RETURN_NOT_OK(doc_write_batch->DeleteSubDoc(sub_path,
                                                  request_.query_id(),
                                                  request_.user_timestamp_usec()));
    }

    // Delete the liveness column as well.
    const DocPath liveness_column(
        row_path.encoded_doc_key(),
        PrimitiveValue::SystemColumnId(SystemColumnIds::kLivenessColumn));
    RETURN_NOT_OK(doc_write_batch->DeleteSubDoc(liveness_column,
                                                request_.query_id(),
                                                request_.user_timestamp_usec()));
  } else {
    RETURN_NOT_OK(doc_write_batch->DeleteSubDoc(row_path));
  }

  return Status::OK();
}

Status QLReadOperation::Execute(const common::QLStorageIf& ql_storage,
                                const ReadHybridTime& read_time,
                                const Schema& schema,
                                const Schema& query_schema,
                                QLResultSet* resultset,
                                HybridTime* restart_read_ht) {
  size_t row_count_limit = std::numeric_limits<std::size_t>::max();
  if (request_.has_limit()) {
    if (request_.limit() == 0) {
      return Status::OK();
    }
    row_count_limit = request_.limit();
  }

  // Create the projections of the non-key columns selected by the row block plus any referenced in
  // the WHERE condition. When DocRowwiseIterator::NextRow() populates the value map, it uses this
  // projection only to scan sub-documents. The query schema is used to select only referenced
  // columns and key columns.
  Schema static_projection, non_static_projection;
  RETURN_NOT_OK(CreateProjections(schema, request_.column_refs(),
                                  &static_projection, &non_static_projection));
  const bool read_static_columns = !static_projection.columns().empty();
  const bool read_distinct_columns = request_.distinct();

  std::unique_ptr<common::QLRowwiseIteratorIf> iter;
  std::unique_ptr<common::QLScanSpec> spec, static_row_spec;
  ReadHybridTime req_read_time;
  RETURN_NOT_OK(ql_storage.BuildQLScanSpec(
      request_, read_time, schema, read_static_columns, static_projection, &spec,
      &static_row_spec, &req_read_time));
  RETURN_NOT_OK(ql_storage.GetIterator(request_, query_schema, schema, txn_op_context_,
                                       req_read_time, &iter));
  RETURN_NOT_OK(iter->Init(*spec));
  if (FLAGS_trace_docdb_calls) {
    TRACE("Initialized iterator");
  }

  QLTableRow static_row;
  QLTableRow non_static_row;
  QLTableRow& selected_row = read_distinct_columns ? static_row : non_static_row;

  // In case when we are continuing a select with a paging state, the static columns for the next
  // row to fetch are not included in the first iterator and we need to fetch them with a separate
  // spec and iterator before beginning the normal fetch below.
  if (static_row_spec != nullptr) {
    std::unique_ptr<common::QLRowwiseIteratorIf> static_row_iter;
    RETURN_NOT_OK(ql_storage.GetIterator(request_, static_projection, schema, txn_op_context_,
                                         req_read_time, &static_row_iter));
    RETURN_NOT_OK(static_row_iter->Init(*static_row_spec));
    if (static_row_iter->HasNext()) {
      RETURN_NOT_OK(static_row_iter->NextRow(&static_row));
    }
  }

  // Begin the normal fetch.
  int match_count = 0;
  bool static_dealt_with = true;
  while (resultset->rsrow_count() < row_count_limit && iter->HasNext()) {
    const bool last_read_static = iter->IsNextStaticColumn();

    // Note that static columns are sorted before non-static columns in DocDB as follows. This is
    // because "<empty_range_components>" is empty and terminated by kGroupEnd which sorts before
    // all other ValueType characters in a non-empty range component.
    //   <hash_code><hash_components><empty_range_components><static_column_id> -> value;
    //   <hash_code><hash_components><range_components><non_static_column_id> -> value;
    if (last_read_static) {
      static_row.Clear();
      RETURN_NOT_OK(iter->NextRow(static_projection, &static_row));
    } else { // Reading a regular row that contains non-static columns.

      // Read this regular row.
      // TODO(omer): this is quite inefficient if read_distinct_column. A better way to do this
      // would be to only read the first non-static column for each hash key, and skip the rest
      non_static_row.Clear();
      RETURN_NOT_OK(iter->NextRow(non_static_projection, &non_static_row));
    }

    // We have two possible cases: whether we use distinct or not
    // If we use distinct, then in general we only need to add the static rows
    // However, we might have to add non-static rows, if there is no static row corresponding to
    // it. Of course, we add one entry per hash key in non-static row.
    // If we do not use distinct, we are generally only adding non-static rows
    // However, if there is no non-static row for the static row, we have to add it.
    if (read_distinct_columns) {
      bool join_successful = false;
      if (!last_read_static) {
        join_successful = JoinNonStaticRow(schema, static_projection, non_static_row, &static_row);
      }

      // If the join was not successful, it means that the non-static row we read has no
      // corresponding static row, so we have to add it to the result
      if (!join_successful) {
        RETURN_NOT_OK(AddRowToResult(
            spec, static_row, row_count_limit, resultset, &match_count));
      }
    } else {
      if (last_read_static) {
        // If the next row to be read is not static, deal with it later, as we do not know whether
        // the non-static row corresponds to this static row; if the non-static row doesn't
        // correspond to this static row, we will have to add it later, so set static_dealt_with to
        // false
        if (iter->HasNext() && !iter->IsNextStaticColumn()) {
          static_dealt_with = false;
          continue;
        }

        AddProjection(non_static_projection, &static_row);
        RETURN_NOT_OK(AddRowToResult(
            spec, static_row, row_count_limit, resultset, &match_count));
      } else {
        // We also have to do the join if we are not reading any static columns, as Cassandra
        // reports nulls for static rows with no corresponding non-static row
        if (read_static_columns || !static_dealt_with) {
          const bool join_successful = JoinStaticRow(schema,
                                               static_projection,
                                               static_row,
                                               &non_static_row);
          // Add the static row is the join was not successful and it is the first time we are
          // dealing with this static row
          if (!join_successful && !static_dealt_with) {
            AddProjection(non_static_projection, &static_row);
            RETURN_NOT_OK(AddRowToResult(
                spec, static_row, row_count_limit, resultset, &match_count));
          }
        }
        static_dealt_with = true;
        RETURN_NOT_OK(AddRowToResult(
            spec, non_static_row, row_count_limit, resultset, &match_count));
      }
    }
  }

  if (request_.is_aggregate() && match_count > 0) {
    RETURN_NOT_OK(PopulateAggregate(selected_row, resultset));
  }

  if (FLAGS_trace_docdb_calls) {
    TRACE("Fetched $0 rows.", resultset->rsrow_count());
  }
  *restart_read_ht = iter->RestartReadHt();

  if (resultset->rsrow_count() >= row_count_limit && !request_.is_aggregate()) {
    RETURN_NOT_OK(iter->SetPagingStateIfNecessary(request_, &response_));
  }

  return Status::OK();
}

CHECKED_STATUS QLReadOperation::PopulateResultSet(const QLTableRow& table_row,
                                                  QLResultSet *resultset) {
  int column_count = request_.selected_exprs().size();
  QLRSRow *rsrow = resultset->AllocateRSRow(column_count);

  int rscol_index = 0;
  for (const QLExpressionPB& expr : request_.selected_exprs()) {
    RETURN_NOT_OK(EvalExpr(expr, table_row, rsrow->rscol(rscol_index)));
    rscol_index++;
  }

  return Status::OK();
}

CHECKED_STATUS QLReadOperation::EvalAggregate(const QLTableRow& table_row) {
  if (aggr_result_.empty()) {
    int column_count = request_.selected_exprs().size();
    aggr_result_.resize(column_count);
  }

  int aggr_index = 0;
  for (const QLExpressionPB& expr : request_.selected_exprs()) {
    RETURN_NOT_OK(EvalExpr(expr, table_row, &aggr_result_[aggr_index]));
    aggr_index++;
  }
  return Status::OK();
}

CHECKED_STATUS QLReadOperation::PopulateAggregate(const QLTableRow& table_row,
                                                  QLResultSet *resultset) {
  int column_count = request_.selected_exprs().size();
  QLRSRow *rsrow = resultset->AllocateRSRow(column_count);
  for (int rscol_index = 0; rscol_index < column_count; rscol_index++) {
    *rsrow->rscol(rscol_index) = aggr_result_[rscol_index];
  }
  return Status::OK();
}

CHECKED_STATUS QLReadOperation::AddRowToResult(const std::unique_ptr<common::QLScanSpec>& spec,
                                               const QLTableRow& row,
                                               const size_t row_count_limit,
                                               QLResultSet* resultset,
                                               int* match_count) {
  if (resultset->rsrow_count() < row_count_limit) {
    bool match = false;
    RETURN_NOT_OK(spec->Match(row, &match));
    if (match) {
      (*match_count)++;
      if (request_.is_aggregate()) {
        RETURN_NOT_OK(EvalAggregate(row));
      } else {
        RETURN_NOT_OK(PopulateResultSet(row, resultset));
      }
    }
  }

  return Status::OK();
}
}  // namespace docdb
}  // namespace yb
