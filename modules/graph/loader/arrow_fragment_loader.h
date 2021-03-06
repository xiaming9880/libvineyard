/** Copyright 2020 Alibaba Group Holding Limited.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#ifndef MODULES_GRAPH_LOADER_ARROW_FRAGMENT_LOADER_H_
#define MODULES_GRAPH_LOADER_ARROW_FRAGMENT_LOADER_H_

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "arrow/util/config.h"
#include "arrow/util/key_value_metadata.h"

#include "grape/worker/comm_spec.h"

#include "basic/stream/dataframe_stream.h"
#include "basic/stream/parallel_stream.h"
#include "client/client.h"
#include "io/io/io_factory.h"
#include "io/io/local_io_adaptor.h"

#include "graph/fragment/arrow_fragment.h"
#include "graph/fragment/arrow_fragment_group.h"
#include "graph/fragment/graph_schema.h"
#include "graph/fragment/property_graph_types.h"
#include "graph/fragment/property_graph_utils.h"
#include "graph/loader/basic_arrow_fragment_loader.h"
#include "graph/utils/error.h"
#include "graph/utils/partitioner.h"
#include "graph/vertex_map/arrow_vertex_map.h"

#define HASH_PARTITION

namespace grape {
inline grape::InArchive& operator<<(grape::InArchive& in_archive,
                                    std::shared_ptr<arrow::Schema>& schema) {
  if (schema != nullptr) {
    std::shared_ptr<arrow::Buffer> out;
#if defined(ARROW_VERSION) && ARROW_VERSION < 17000
    CHECK_ARROW_ERROR(arrow::ipc::SerializeSchema(
        *schema, nullptr, arrow::default_memory_pool(), &out));
#elif defined(ARROW_VERSION) && ARROW_VERSION < 2000000
    CHECK_ARROW_ERROR_AND_ASSIGN(
        out, arrow::ipc::SerializeSchema(*schema, nullptr,
                                         arrow::default_memory_pool()));
#else
    CHECK_ARROW_ERROR_AND_ASSIGN(
        out,
        arrow::ipc::SerializeSchema(*schema, arrow::default_memory_pool()));
#endif
    in_archive.AddBytes(out->data(), out->size());
  }
  return in_archive;
}

inline grape::OutArchive& operator>>(grape::OutArchive& out_archive,
                                     std::shared_ptr<arrow::Schema>& schema) {
  if (!out_archive.Empty()) {
    auto buffer = std::make_shared<arrow::Buffer>(
        reinterpret_cast<const uint8_t*>(out_archive.GetBuffer()),
        out_archive.GetSize());
    arrow::io::BufferReader reader(buffer);
#if defined(ARROW_VERSION) && ARROW_VERSION < 17000
    CHECK_ARROW_ERROR(arrow::ipc::ReadSchema(&reader, nullptr, &schema));
#else
    CHECK_ARROW_ERROR_AND_ASSIGN(schema,
                                 arrow::ipc::ReadSchema(&reader, nullptr));
#endif
  }
  return out_archive;
}
}  // namespace grape

namespace vineyard {

template <typename OID_T = property_graph_types::OID_TYPE,
          typename VID_T = property_graph_types::VID_TYPE>
class ArrowFragmentLoader {
  using oid_t = OID_T;
  using vid_t = VID_T;
  using label_id_t = property_graph_types::LABEL_ID_TYPE;
  using internal_oid_t = typename InternalType<oid_t>::type;
  using oid_array_t = typename vineyard::ConvertToArrowType<oid_t>::ArrayType;
  using vertex_map_t = ArrowVertexMap<internal_oid_t, vid_t>;
  // These consts represent which column in the arrow table represents oid.
  const int id_column = 0;
  const int src_column = 0;
  const int dst_column = 1;
  // These consts represent the key in the path of vfile/efile
  static constexpr const char* LABEL_TAG = "label";
  static constexpr const char* SRC_LABEL_TAG = "src_label";
  static constexpr const char* DST_LABEL_TAG = "dst_label";
#ifdef HASH_PARTITION
  using partitioner_t = HashPartitioner<oid_t>;
#else
  using partitioner_t = SegmentedPartitioner<oid_t>;
#endif
  using basic_loader_t = BasicArrowFragmentLoader<oid_t, vid_t, partitioner_t>;

 public:
  /**
   *
   * @param client
   * @param comm_spec
   * @param efiles An example of efile:
   * /data/twitter_e_0_0_0#src_label=v0&dst_label=v0&label=e0;/data/twitter_e_0_1_0#src_label=v0&dst_label=v1&label=e0;/data/twitter_e_1_0_0#src_label=v1&dst_label=v0&label=e0;/data/twitter_e_1_1_0#src_label=v1&dst_label=v1&label=e0
   * @param vfiles An example of vfile: /data/twitter_v_0#label=v0
   * @param directed
   */
  ArrowFragmentLoader(vineyard::Client& client,
                      const grape::CommSpec& comm_spec,
                      const std::vector<std::string>& efiles,
                      const std::vector<std::string>& vfiles,
                      bool directed = true)
      : client_(client),
        comm_spec_(comm_spec),
        efiles_(efiles),
        vfiles_(vfiles),
        vertex_label_num_(vfiles.size()),
        edge_label_num_(efiles.size()),
        directed_(directed),
        basic_arrow_fragment_loader_(comm_spec) {}

  ArrowFragmentLoader(vineyard::Client& client,
                      const grape::CommSpec& comm_spec,
                      const std::vector<ObjectID>& vstreams,
                      const std::vector<std::vector<ObjectID>>& estreams,
                      bool directed = true)
      : client_(client),
        comm_spec_(comm_spec),
        vertex_label_num_(vstreams.size()),
        edge_label_num_(estreams.size()),
        directed_(directed),
        basic_arrow_fragment_loader_(comm_spec) {
    partial_v_tables_ = gatherVTables(client, vstreams);
    partial_e_tables_ = gatherETables(client, estreams);
  }

  ArrowFragmentLoader(
      vineyard::Client& client, const grape::CommSpec& comm_spec,
      label_id_t vertex_label_num, label_id_t edge_label_num,
      std::vector<std::shared_ptr<arrow::Table>> const& partial_v_tables,
      std::vector<std::vector<std::shared_ptr<arrow::Table>>> const&
          partial_e_tables,
      bool directed = true)
      : client_(client),
        comm_spec_(comm_spec),
        vertex_label_num_(vertex_label_num),
        edge_label_num_(edge_label_num),
        partial_v_tables_(partial_v_tables),
        partial_e_tables_(partial_e_tables),
        directed_(directed),
        basic_arrow_fragment_loader_(comm_spec) {}

  ArrowFragmentLoader(vineyard::Client& client,
                      const grape::CommSpec& comm_spec,
                      const std::vector<std::string>& efiles,
                      bool directed = true)
      : client_(client),
        comm_spec_(comm_spec),
        efiles_(efiles),
        vfiles_(),
        vertex_label_num_(0),
        edge_label_num_(efiles.size()),
        directed_(directed),
        basic_arrow_fragment_loader_(comm_spec) {}

  ~ArrowFragmentLoader() = default;

  boost::leaf::result<vineyard::ObjectID> LoadFragment() {
    BOOST_LEAF_CHECK(initPartitioner());
    BOOST_LEAF_CHECK(initBasicLoader());
    BOOST_LEAF_AUTO(frag_id, shuffleAndBuild());
    return frag_id;
  }

  boost::leaf::result<vineyard::ObjectID> LoadFragmentAsFragmentGroup() {
    BOOST_LEAF_AUTO(frag_id, LoadFragment());
    BOOST_LEAF_AUTO(group_id,
                    constructFragmentGroup(client_, frag_id, comm_spec_,
                                           vertex_label_num_, edge_label_num_));
    return group_id;
  }

 protected:  // for subclasses
  boost::leaf::result<void> initPartitioner() {
#ifdef HASH_PARTITION
    partitioner_.Init(comm_spec_.fnum());
#else
    if (vfiles_.empty()) {
      RETURN_GS_ERROR(
          ErrorCode::kInvalidOperationError,
          "Segmented partitioner is not supported when the v-file is "
          "not provided");
    }
    std::vector<std::shared_ptr<arrow::Table>> vtables;
    {
      BOOST_LEAF_AUTO(tmp, loadVertexTables(vfiles_, 0, 1));
      vtables = tmp;
    }
    std::vector<oid_t> oid_list;

    for (auto& table : vtables) {
      std::shared_ptr<arrow::ChunkedArray> oid_array_chunks =
          table->column(id_column);
      size_t chunk_num = oid_array_chunks->num_chunks();

      for (size_t chunk_i = 0; chunk_i != chunk_num; ++chunk_i) {
        std::shared_ptr<oid_array_t> array =
            std::dynamic_pointer_cast<oid_array_t>(
                oid_array_chunks->chunk(chunk_i));
        int64_t length = array->length();
        for (int64_t i = 0; i < length; ++i) {
          oid_list.emplace_back(oid_t(array->GetView(i)));
        }
      }
    }

    partitioner_.Init(comm_spec_.fnum(), oid_list);
#endif
    return {};
  }

  boost::leaf::result<void> initBasicLoader() {
    std::vector<std::shared_ptr<arrow::Table>> partial_v_tables;
    std::vector<std::vector<std::shared_ptr<arrow::Table>>> partial_e_tables;
    if (!partial_v_tables_.empty() && !partial_e_tables_.empty()) {
      partial_v_tables = partial_v_tables_;
      partial_e_tables = partial_e_tables_;
    } else {
      // if vfiles is empty, we infer oids from efile
      if (vfiles_.empty()) {
        auto load_procedure = [&]() {
          return loadEVTablesFromEFiles(efiles_, comm_spec_.worker_id(),
                                        comm_spec_.worker_num());
        };
        BOOST_LEAF_AUTO(ev_tables, sync_gs_error(comm_spec_, load_procedure));
        partial_v_tables = ev_tables.first;
        partial_e_tables = ev_tables.second;
      } else {
        auto load_v_procedure = [&]() {
          return loadVertexTables(vfiles_, comm_spec_.worker_id(),
                                  comm_spec_.worker_num());
        };
        BOOST_LEAF_AUTO(tmp_v, sync_gs_error(comm_spec_, load_v_procedure));
        partial_v_tables = tmp_v;
        auto load_e_procedure = [&]() {
          return loadEdgeTables(efiles_, comm_spec_.worker_id(),
                                comm_spec_.worker_num());
        };
        BOOST_LEAF_AUTO(tmp_e, sync_gs_error(comm_spec_, load_e_procedure));
        partial_e_tables = tmp_e;
      }
    }
    basic_arrow_fragment_loader_.Init(partial_v_tables, partial_e_tables);
    basic_arrow_fragment_loader_.SetPartitioner(partitioner_);

    return {};
  }

  boost::leaf::result<vineyard::ObjectID> shuffleAndBuild() {
    // When vfiles_ is empty, it means we build vertex table from efile
    BOOST_LEAF_AUTO(
        local_v_tables,
        basic_arrow_fragment_loader_.ShuffleVertexTables(vfiles_.empty()));
    auto oid_lists = basic_arrow_fragment_loader_.GetOidLists();

    BasicArrowVertexMapBuilder<typename InternalType<oid_t>::type, vid_t>
        vm_builder(client_, comm_spec_.fnum(), vertex_label_num_, oid_lists);
    auto vm = vm_builder.Seal(client_);
    auto vm_ptr =
        std::dynamic_pointer_cast<vertex_map_t>(client_.GetObject(vm->id()));
    auto mapper = [&vm_ptr](fid_t fid, label_id_t label, internal_oid_t oid,
                            vid_t& gid) {
      CHECK(vm_ptr->GetGid(fid, label, oid, gid));
      return true;
    };
    BOOST_LEAF_AUTO(local_e_tables,
                    basic_arrow_fragment_loader_.ShuffleEdgeTables(mapper));
    BasicArrowFragmentBuilder<oid_t, vid_t> frag_builder(client_, vm_ptr);
    PropertyGraphSchema schema;

    schema.set_fnum(comm_spec_.fnum());

    {
      std::vector<std::string> vertex_label_list(vertex_label_num_);
      std::vector<bool> vertex_label_bitset(vertex_label_num_, false);
      for (auto& pair : vertex_label_to_index_) {
        if (pair.second > vertex_label_num_) {
          return boost::leaf::new_error(ErrorCode::kIOError,
                                        "Failed to map vertex label to index");
        }
        if (vertex_label_bitset[pair.second]) {
          return boost::leaf::new_error(
              ErrorCode::kIOError,
              "Multiple vertex labels are mapped to one index.");
        }
        vertex_label_bitset[pair.second] = true;
        vertex_label_list[pair.second] = pair.first;
      }
      for (label_id_t v_label = 0; v_label != vertex_label_num_; ++v_label) {
        std::string vertex_label = vertex_label_list[v_label];
        auto entry = schema.CreateEntry(vertex_label, "VERTEX");

        std::unordered_map<std::string, std::string> kvs;
        auto table = local_v_tables[v_label];
        table->schema()->metadata()->ToUnorderedMap(&kvs);

        entry->AddPrimaryKeys(1, std::vector<std::string>{kvs["primary_key"]});

        // N.B. ID column is not removed, and we need that
        for (int64_t i = 0; i < table->num_columns(); ++i) {
          entry->AddProperty(table->schema()->field(i)->name(),
                             table->schema()->field(i)->type());
        }
      }
    }

    {
      std::vector<std::string> edge_label_list(edge_label_num_);
      std::vector<bool> edge_label_bitset(edge_label_num_, false);
      for (auto& pair : edge_label_to_index_) {
        if (pair.second > edge_label_num_) {
          return boost::leaf::new_error(ErrorCode::kIOError,
                                        "Failed to map edge label to index");
        }
        if (edge_label_bitset[pair.second]) {
          return boost::leaf::new_error(
              ErrorCode::kIOError,
              "Multiple edge labels are mapped to one index.");
        }
        edge_label_bitset[pair.second] = true;
        edge_label_list[pair.second] = pair.first;
      }
      for (label_id_t e_label = 0; e_label != edge_label_num_; ++e_label) {
        std::string edge_label = edge_label_list[e_label];
        auto entry = schema.CreateEntry(edge_label, "EDGE");
        auto& pairs = edge_vertex_label_.at(edge_label);
        for (auto& vpair : pairs) {
          std::string src_label = vpair.first;
          std::string dst_label = vpair.second;
          entry->AddRelation(src_label, dst_label);
        }

        auto table = local_e_tables.at(e_label);
        for (int64_t i = 2; i < table->num_columns(); ++i) {
          entry->AddProperty(table->schema()->field(i)->name(),
                             table->schema()->field(i)->type());
        }
      }
    }

    frag_builder.SetPropertyGraphSchema(std::move(schema));

    int thread_num =
        (std::thread::hardware_concurrency() + comm_spec_.local_num() - 1) /
        comm_spec_.local_num();
    BOOST_LEAF_CHECK(frag_builder.Init(
        comm_spec_.fid(), comm_spec_.fnum(), std::move(local_v_tables),
        std::move(local_e_tables), directed_, thread_num));
    auto frag = std::dynamic_pointer_cast<ArrowFragment<oid_t, vid_t>>(
        frag_builder.Seal(client_));
    VINEYARD_CHECK_OK(client_.Persist(frag->id()));
    return frag->id();
  }

  boost::leaf::result<vineyard::ObjectID> constructFragmentGroup(
      vineyard::Client& client, vineyard::ObjectID frag_id,
      const grape::CommSpec& comm_spec, label_id_t v_label_num,
      label_id_t e_label_num) {
    vineyard::ObjectID group_object_id;
    uint64_t instance_id = client.instance_id();

    if (comm_spec.worker_id() == 0) {
      std::vector<uint64_t> gathered_instance_ids(comm_spec.worker_num());
      std::vector<vineyard::ObjectID> gathered_object_ids(
          comm_spec.worker_num());

      MPI_Gather(&instance_id, sizeof(uint64_t), MPI_CHAR,
                 &gathered_instance_ids[0], sizeof(uint64_t), MPI_CHAR, 0,
                 comm_spec.comm());

      MPI_Gather(&frag_id, sizeof(vineyard::ObjectID), MPI_CHAR,
                 &gathered_object_ids[0], sizeof(vineyard::ObjectID), MPI_CHAR,
                 0, comm_spec.comm());

      ArrowFragmentGroupBuilder builder;
      builder.set_total_frag_num(comm_spec.fnum());
      builder.set_vertex_label_num(v_label_num);
      builder.set_edge_label_num(e_label_num);
      for (fid_t i = 0; i < comm_spec.fnum(); ++i) {
        builder.AddFragmentObject(
            i, gathered_object_ids[comm_spec.FragToWorker(i)],
            gathered_instance_ids[comm_spec.FragToWorker(i)]);
      }

      auto group_object =
          std::dynamic_pointer_cast<ArrowFragmentGroup>(builder.Seal(client));
      group_object_id = group_object->id();
      VY_OK_OR_RAISE(client.Persist(group_object_id));

      MPI_Bcast(&group_object_id, sizeof(vineyard::ObjectID), MPI_CHAR, 0,
                comm_spec.comm());

    } else {
      MPI_Gather(&instance_id, sizeof(uint64_t), MPI_CHAR, NULL,
                 sizeof(uint64_t), MPI_CHAR, 0, comm_spec.comm());
      MPI_Gather(&frag_id, sizeof(vineyard::ObjectID), MPI_CHAR, NULL,
                 sizeof(vineyard::ObjectID), MPI_CHAR, 0, comm_spec.comm());

      MPI_Bcast(&group_object_id, sizeof(vineyard::ObjectID), MPI_CHAR, 0,
                comm_spec.comm());
    }
    return group_object_id;
  }

  boost::leaf::result<std::vector<std::shared_ptr<arrow::Table>>>
  loadVertexTables(const std::vector<std::string>& files, int index,
                   int total_parts) {
    auto label_num = static_cast<label_id_t>(files.size());
    std::vector<std::shared_ptr<arrow::Table>> tables(label_num);

    for (label_id_t label_id = 0; label_id < label_num; ++label_id) {
      std::unique_ptr<vineyard::LocalIOAdaptor,
                      std::function<void(vineyard::LocalIOAdaptor*)>>
          io_adaptor(new vineyard::LocalIOAdaptor(files[label_id] +
                                                  "#header_row=true"),
                     io_deleter_);
      auto read_procedure =
          [&]() -> boost::leaf::result<std::shared_ptr<arrow::Table>> {
        VY_OK_OR_RAISE(io_adaptor->SetPartialRead(index, total_parts));
        VY_OK_OR_RAISE(io_adaptor->Open());
        std::shared_ptr<arrow::Table> table;
        VY_OK_OR_RAISE(io_adaptor->ReadTable(&table));
        return table;
      };

      BOOST_LEAF_AUTO(table, sync_gs_error(comm_spec_, read_procedure));

      auto sync_schema_procedure =
          [&]() -> boost::leaf::result<std::shared_ptr<arrow::Table>> {
        return SyncSchema(table, comm_spec_);
      };

      BOOST_LEAF_AUTO(normalized_table,
                      sync_gs_error(comm_spec_, sync_schema_procedure));

      auto meta = std::make_shared<arrow::KeyValueMetadata>();

      meta->Append("type", "VERTEX");
      meta->Append(basic_loader_t::ID_COLUMN, std::to_string(id_column));

      auto adaptor_meta = io_adaptor->GetMeta();
      for (auto const& kv : adaptor_meta) {
        meta->Append(kv.first, kv.second);
      }
      // If label name is not in meta, we assign a default label '_'
      if (adaptor_meta.find(LABEL_TAG) == adaptor_meta.end()) {
        RETURN_GS_ERROR(
            ErrorCode::kIOError,
            "Metadata of input vertex files should contain label name");
      }
      auto v_label_name = adaptor_meta.find(LABEL_TAG)->second;
      meta->Append("label", v_label_name);
      tables[label_id] = normalized_table->ReplaceSchemaMetadata(meta);

      vertex_label_to_index_[v_label_name] = label_id;
    }
    return tables;
  }

  boost::leaf::result<std::vector<std::vector<std::shared_ptr<arrow::Table>>>>
  loadEdgeTables(const std::vector<std::string>& files, int index,
                 int total_parts) {
    auto label_num = static_cast<label_id_t>(files.size());
    std::vector<std::vector<std::shared_ptr<arrow::Table>>> tables(label_num);

    try {
      for (label_id_t label_id = 0; label_id < label_num; ++label_id) {
        std::vector<std::string> sub_label_files;
        boost::split(sub_label_files, files[label_id], boost::is_any_of(";"));

        for (size_t j = 0; j < sub_label_files.size(); ++j) {
          std::unique_ptr<vineyard::LocalIOAdaptor,
                          std::function<void(vineyard::LocalIOAdaptor*)>>
              io_adaptor(new vineyard::LocalIOAdaptor(sub_label_files[j] +
                                                      "#header_row=true"),
                         io_deleter_);
          auto read_procedure =
              [&]() -> boost::leaf::result<std::shared_ptr<arrow::Table>> {
            VY_OK_OR_RAISE(io_adaptor->SetPartialRead(index, total_parts));
            VY_OK_OR_RAISE(io_adaptor->Open());
            std::shared_ptr<arrow::Table> table;
            VY_OK_OR_RAISE(io_adaptor->ReadTable(&table));
            return table;
          };
          BOOST_LEAF_AUTO(table, sync_gs_error(comm_spec_, read_procedure));

          auto sync_schema_procedure =
              [&]() -> boost::leaf::result<std::shared_ptr<arrow::Table>> {
            return SyncSchema(table, comm_spec_);
          };
          BOOST_LEAF_AUTO(normalized_table,
                          sync_gs_error(comm_spec_, sync_schema_procedure));

          std::shared_ptr<arrow::KeyValueMetadata> meta(
              new arrow::KeyValueMetadata());
          meta->Append("type", "EDGE");
          meta->Append(basic_loader_t::SRC_COLUMN, std::to_string(src_column));
          meta->Append(basic_loader_t::DST_COLUMN, std::to_string(dst_column));
          meta->Append("sub_label_num", std::to_string(sub_label_files.size()));

          auto adaptor_meta = io_adaptor->GetMeta();
          auto it = adaptor_meta.find(LABEL_TAG);
          if (it == adaptor_meta.end()) {
            RETURN_GS_ERROR(
                ErrorCode::kIOError,
                "Metadata of input edge files should contain label name");
          }
          std::string edge_label_name = it->second;
          meta->Append("label", edge_label_name);

          it = adaptor_meta.find(SRC_LABEL_TAG);
          if (it == adaptor_meta.end()) {
            RETURN_GS_ERROR(
                ErrorCode::kIOError,
                "Metadata of input edge files should contain src label name");
          }
          std::string src_label_name = it->second;
          meta->Append(
              basic_loader_t::SRC_LABEL_ID,
              std::to_string(vertex_label_to_index_.at(src_label_name)));

          it = adaptor_meta.find(DST_LABEL_TAG);
          if (it == adaptor_meta.end()) {
            RETURN_GS_ERROR(
                ErrorCode::kIOError,
                "Metadata of input edge files should contain dst label name");
          }
          std::string dst_label_name = it->second;
          meta->Append(
              basic_loader_t::DST_LABEL_ID,
              std::to_string(vertex_label_to_index_.at(dst_label_name)));

          tables[label_id].emplace_back(
              normalized_table->ReplaceSchemaMetadata(meta));
          edge_vertex_label_[edge_label_name].insert(
              std::make_pair(src_label_name, dst_label_name));
          edge_label_to_index_[edge_label_name] = label_id;
        }
      }
    } catch (std::exception& e) {
      RETURN_GS_ERROR(ErrorCode::kIOError, std::string(e.what()));
    }
    return tables;
  }

  boost::leaf::result<
      std::pair<std::vector<std::shared_ptr<arrow::Table>>,
                std::vector<std::vector<std::shared_ptr<arrow::Table>>>>>
  loadEVTablesFromEFiles(const std::vector<std::string>& efiles, int index,
                         int total_parts) {
    std::vector<std::string> vertex_label_names;
    {
      std::set<std::string> vertex_label_name_set;

      // We don't open file, just get metadata from filename
      for (auto& efile : efiles) {
        std::vector<std::string> sub_label_files;
        // for each type of edge, efile is separated by ;
        boost::split(sub_label_files, efile, boost::is_any_of(";"));

        for (auto& sub_efile : sub_label_files) {
          std::unique_ptr<vineyard::LocalIOAdaptor,
                          std::function<void(vineyard::LocalIOAdaptor*)>>
              io_adaptor(
                  new vineyard::LocalIOAdaptor(sub_efile + "#header_row=true"),
                  io_deleter_);
          auto meta = io_adaptor->GetMeta();
          auto src_label_name = meta.find(SRC_LABEL_TAG);
          auto dst_label_name = meta.find(DST_LABEL_TAG);

          if (src_label_name == meta.end() || dst_label_name == meta.end()) {
            RETURN_GS_ERROR(
                ErrorCode::kIOError,
                "Metadata of input edge files should contain label name");
          } else {
            vertex_label_name_set.insert(src_label_name->second);
            vertex_label_name_set.insert(dst_label_name->second);
          }
        }
      }

      vertex_label_num_ = vertex_label_name_set.size();
      vertex_label_names.resize(vertex_label_num_);
      // number label id
      label_id_t v_label_id = 0;
      for (auto& vertex_name : vertex_label_name_set) {
        vertex_label_to_index_[vertex_name] = v_label_id;
        vertex_label_names[v_label_id] = vertex_name;
        v_label_id++;
      }
    }

    std::vector<std::vector<std::shared_ptr<arrow::Table>>> etables(
        edge_label_num_);
    std::vector<OidSet<oid_t>> oids(vertex_label_num_);

    try {
      for (label_id_t e_label_id = 0; e_label_id < edge_label_num_;
           ++e_label_id) {
        std::vector<std::string> sub_label_files;
        boost::split(sub_label_files, efiles[e_label_id],
                     boost::is_any_of(";"));

        for (auto& sub_efile : sub_label_files) {
          std::unique_ptr<vineyard::LocalIOAdaptor,
                          std::function<void(vineyard::LocalIOAdaptor*)>>
              io_adaptor(
                  new vineyard::LocalIOAdaptor(sub_efile + "#header_row=true"),
                  io_deleter_);

          auto read_procedure =
              [&]() -> boost::leaf::result<std::shared_ptr<arrow::Table>> {
            VY_OK_OR_RAISE(io_adaptor->SetPartialRead(index, total_parts));
            VY_OK_OR_RAISE(io_adaptor->Open());
            std::shared_ptr<arrow::Table> table;
            VY_OK_OR_RAISE(io_adaptor->ReadTable(&table));
            return table;
          };

          BOOST_LEAF_AUTO(table, sync_gs_error(comm_spec_, read_procedure));

          auto sync_schema_procedure =
              [&]() -> boost::leaf::result<std::shared_ptr<arrow::Table>> {
            return SyncSchema(table, comm_spec_);
          };

          BOOST_LEAF_AUTO(normalized_table,
                          sync_gs_error(comm_spec_, sync_schema_procedure));

          auto adaptor_meta = io_adaptor->GetMeta();
          auto it = adaptor_meta.find(LABEL_TAG);
          if (it == adaptor_meta.end()) {
            RETURN_GS_ERROR(
                ErrorCode::kIOError,
                "Metadata of input edge files should contain label name");
          }

          std::shared_ptr<arrow::KeyValueMetadata> meta(
              new arrow::KeyValueMetadata());
          meta->Append("type", "EDGE");
          meta->Append(basic_loader_t::SRC_COLUMN, std::to_string(src_column));
          meta->Append(basic_loader_t::DST_COLUMN, std::to_string(dst_column));
          meta->Append("sub_label_num", std::to_string(sub_label_files.size()));

          std::string edge_label_name = it->second;
          meta->Append("label", edge_label_name);

          it = adaptor_meta.find(SRC_LABEL_TAG);
          if (it == adaptor_meta.end()) {
            RETURN_GS_ERROR(
                ErrorCode::kIOError,
                "Metadata of input edge files should contain src label name");
          }
          std::string src_label_name = it->second;
          auto src_label_id = vertex_label_to_index_.at(src_label_name);

          meta->Append(basic_loader_t::SRC_LABEL_ID,
                       std::to_string(src_label_id));
          it = adaptor_meta.find(DST_LABEL_TAG);

          if (it == adaptor_meta.end()) {
            RETURN_GS_ERROR(
                ErrorCode::kIOError,
                "Metadata of input edge files should contain dst label name");
          }

          std::string dst_label_name = it->second;
          auto dst_label_id = vertex_label_to_index_.at(dst_label_name);

          meta->Append(basic_loader_t::DST_LABEL_ID,
                       std::to_string(dst_label_id));
          auto e_table = normalized_table->ReplaceSchemaMetadata(meta);

          etables[e_label_id].emplace_back(e_table);
          edge_vertex_label_[edge_label_name].insert(
              std::make_pair(src_label_name, dst_label_name));
          if (edge_label_to_index_.find(edge_label_name) ==
              edge_label_to_index_.end()) {
            edge_label_to_index_[edge_label_name] = e_label_id;
          } else if (edge_label_to_index_[edge_label_name] != e_label_id) {
            RETURN_GS_ERROR(
                ErrorCode::kInvalidValueError,
                "Edge label is not consistent, " + edge_label_name + ": " +
                    std::to_string(e_label_id) + " vs " +
                    std::to_string(edge_label_to_index_[edge_label_name]));
          }

          // Build oid set from etable
          BOOST_LEAF_CHECK(
              oids[src_label_id].BatchInsert(e_table->column(src_column)));
          BOOST_LEAF_CHECK(
              oids[dst_label_id].BatchInsert(e_table->column(dst_column)));
        }
      }
    } catch (std::exception& e) {
      RETURN_GS_ERROR(ErrorCode::kIOError, std::string(e.what()));
    }

    // Now, oids are ready to use
    std::vector<std::shared_ptr<arrow::Table>> vtables(vertex_label_num_);

    for (auto v_label_id = 0; v_label_id < vertex_label_num_; v_label_id++) {
      auto label_name = vertex_label_names[v_label_id];
      std::vector<std::shared_ptr<arrow::Field>> schema_vector{arrow::field(
          label_name, vineyard::ConvertToArrowType<oid_t>::TypeValue())};
      BOOST_LEAF_AUTO(oid_array, oids[v_label_id].ToArrowArray());
      std::vector<std::shared_ptr<arrow::Array>> arrays{oid_array};
      auto schema = std::make_shared<arrow::Schema>(schema_vector);
      auto v_table = arrow::Table::Make(schema, arrays);
      std::shared_ptr<arrow::KeyValueMetadata> meta(
          new arrow::KeyValueMetadata());

      meta->Append("type", "VERTEX");
      meta->Append("label_index", std::to_string(v_label_id));
      meta->Append("label", label_name);
      meta->Append(basic_loader_t::ID_COLUMN, std::to_string(id_column));
      vtables[v_label_id] = v_table->ReplaceSchemaMetadata(meta);
    }
    return std::make_pair(vtables, etables);
  }

  Status readTableFromVineyard(vineyard::Client& client,

                               const ObjectID object_id,
                               std::shared_ptr<arrow::Table>& table) {
    auto pstream = client.GetObject<vineyard::ParallelStream>(object_id);
    RETURN_ON_ASSERT(pstream != nullptr,
                     "Object not exists: " + VYObjectIDToString(object_id));
    int index = comm_spec_.worker_id();
    int total_parts = comm_spec_.worker_num();
    RETURN_ON_ASSERT(
        total_parts == pstream->GetStreamSize() && index < total_parts,
        "read " + std::to_string(index) + " from " +
            std::to_string(total_parts) + ", but totally has " +
            std::to_string(pstream->GetStreamSize()));
    auto dataframe_stream =
        pstream->GetStream<vineyard::DataframeStream>(index);
    RETURN_ON_ASSERT(dataframe_stream != nullptr,
                     "The stream must be a dataframe stream");
    auto reader = dataframe_stream->OpenReader(client);
    RETURN_ON_ERROR(reader->ReadTable(table));
    VLOG(10) << "table from stream: " << table->schema()->ToString();
    return Status::OK();
  }

  std::vector<std::shared_ptr<arrow::Table>> gatherVTables(
      vineyard::Client& client, const std::vector<ObjectID>& vstreams) {
    std::vector<std::shared_ptr<arrow::Table>> tables;
    for (label_id_t label_id = 0; label_id < vstreams.size(); ++label_id) {
      auto const& vstream = vstreams[label_id];
      std::shared_ptr<arrow::Table> table;
      auto status = readTableFromVineyard(client, vstream, table);
      if (status.ok()) {
        std::shared_ptr<arrow::KeyValueMetadata> meta;
        if (table->schema()->metadata() != nullptr) {
          meta = table->schema()->metadata()->Copy();
        } else {
          meta.reset(new arrow::KeyValueMetadata());
        }
        meta->Append("type", "VERTEX");
        meta->Append(basic_loader_t::ID_COLUMN, std::to_string(id_column));
        tables.emplace_back(table->ReplaceSchemaMetadata(meta));

        int label_meta_index = meta->FindKey(LABEL_TAG);
        VINEYARD_ASSERT(
            label_meta_index != -1,
            "Metadata of input vertex files should contain label name");
        vertex_label_to_index_[meta->value(label_meta_index)] = label_id;
      } else {
        LOG(ERROR) << "Failed to read vertex stream: " << status.ToString();
      }
    }
    return tables;
  }

  std::vector<std::vector<std::shared_ptr<arrow::Table>>> gatherETables(
      vineyard::Client& client,
      const std::vector<std::vector<ObjectID>>& estreams) {
    std::vector<std::vector<std::shared_ptr<arrow::Table>>> tables;
    for (label_id_t label_id = 0; label_id < estreams.size(); ++label_id) {
      auto const& esubstreams = estreams[label_id];
      std::vector<std::shared_ptr<arrow::Table>> subtables;
      for (auto const& estream : esubstreams) {
        std::shared_ptr<arrow::Table> table;
        auto status = readTableFromVineyard(client, estream, table);
        if (status.ok()) {
          std::shared_ptr<arrow::KeyValueMetadata> meta;
          if (table->schema()->metadata() != nullptr) {
            meta = table->schema()->metadata()->Copy();
          } else {
            meta.reset(new arrow::KeyValueMetadata());
          }
          meta->Append("type", "EDGE");
          meta->Append(basic_loader_t::SRC_COLUMN, std::to_string(src_column));
          meta->Append(basic_loader_t::DST_COLUMN, std::to_string(dst_column));
          meta->Append("sub_label_num", std::to_string(esubstreams.size()));

          int label_meta_index = meta->FindKey(LABEL_TAG);
          VINEYARD_ASSERT(
              label_meta_index != -1,
              "Metadata of input edge files should contain label name");
          std::string edge_label_name = meta->value(label_meta_index);

          int src_label_meta_index = meta->FindKey(SRC_LABEL_TAG);
          VINEYARD_ASSERT(
              src_label_meta_index != -1,
              "Metadata of input edge files should contain src_label name");
          std::string src_label_name = meta->value(src_label_meta_index);

          int dst_label_meta_index = meta->FindKey(DST_LABEL_TAG);
          VINEYARD_ASSERT(
              dst_label_meta_index != -1,
              "Metadata of input edge files should contain dst_label name");
          std::string dst_label_name = meta->value(dst_label_meta_index);

          meta->Append(
              basic_loader_t::SRC_LABEL_ID,
              std::to_string(vertex_label_to_index_.at(src_label_name)));
          meta->Append(
              basic_loader_t::DST_LABEL_ID,
              std::to_string(vertex_label_to_index_.at(dst_label_name)));

          edge_vertex_label_[edge_label_name].insert(
              std::make_pair(src_label_name, dst_label_name));
          edge_label_to_index_[edge_label_name] = label_id;

          subtables.emplace_back(table->ReplaceSchemaMetadata(meta));
        } else {
          LOG(ERROR) << "Failed to read edge stream: " << status.ToString();
        }
      }
      if (!subtables.empty()) {
        tables.emplace_back(subtables);
      }
    }
    return tables;
  }

  arrow::Status swapColumn(std::shared_ptr<arrow::Table> in, int lhs_index,
                           int rhs_index, std::shared_ptr<arrow::Table>* out) {
    if (lhs_index == rhs_index) {
      out = &in;
      return arrow::Status::OK();
    }
    if (lhs_index > rhs_index) {
      return arrow::Status::Invalid("lhs index must smaller than rhs index.");
    }
    auto field = in->schema()->field(rhs_index);
    auto column = in->column(rhs_index);
#if defined(ARROW_VERSION) && ARROW_VERSION < 17000
    CHECK_ARROW_ERROR(in->RemoveColumn(rhs_index, &in));
    CHECK_ARROW_ERROR(in->AddColumn(lhs_index, field, column, out));
#else
    CHECK_ARROW_ERROR_AND_ASSIGN(in, in->RemoveColumn(rhs_index));
    CHECK_ARROW_ERROR_AND_ASSIGN(*out, in->AddColumn(lhs_index, field, column));
#endif
    return arrow::Status::OK();
  }

  boost::leaf::result<std::shared_ptr<arrow::Schema>> TypeLoosen(
      const std::vector<std::shared_ptr<arrow::Schema>>& schemas) {
    size_t field_num = 0;
    for (const auto& schema : schemas) {
      if (schema != nullptr) {
        field_num = schema->num_fields();
        break;
      }
    }
    if (field_num == 0) {
      RETURN_GS_ERROR(ErrorCode::kInvalidOperationError,
                      "Every schema is empty");
    }
    // Perform type lossen.
    // timestamp -> int64 -> double -> utf8   binary (not supported)
    std::vector<std::vector<std::shared_ptr<arrow::Field>>> fields(field_num);
    for (size_t i = 0; i < field_num; ++i) {
      for (const auto& schema : schemas) {
        if (schema != nullptr) {
          fields[i].push_back(schema->field(i));
        }
      }
    }
    std::vector<std::shared_ptr<arrow::Field>> lossen_fields(field_num);

    for (size_t i = 0; i < field_num; ++i) {
      // find the max frequency using linear traversal
      auto res = fields[i][0]->type();
      if (res->Equals(arrow::timestamp(arrow::TimeUnit::SECOND))) {
        res = arrow::int64();
      }
      if (res->Equals(arrow::int64())) {
        for (size_t j = 1; j < fields[i].size(); ++j) {
          if (fields[i][j]->type()->Equals(arrow::float64())) {
            res = arrow::float64();
          }
        }
      }
      if (res->Equals(arrow::float64())) {
        for (size_t j = 1; j < fields[i].size(); ++j) {
          if (fields[i][j]->type()->Equals(arrow::utf8())) {
            res = arrow::utf8();
          }
        }
      }
      lossen_fields[i] = fields[i][0]->WithType(res);
    }
    return std::make_shared<arrow::Schema>(lossen_fields);
  }

  // This method used when several workers is loading a file in parallel, each
  // worker will read a chunk of the origin file into a arrow::Table.
  // We may get different table schemas as some chunks may have zero rows
  // or some chunks' data doesn't have any floating numbers, but others might
  // have. We could use this method to gather their schemas, and find out most
  // common fields, construct a new schema and broadcast back. Note: We perform
  // type loosen, int64 -> double. timestamp -> int64.
  boost::leaf::result<std::shared_ptr<arrow::Table>> SyncSchema(
      const std::shared_ptr<arrow::Table>& table,
      const grape::CommSpec& comm_spec) {
    std::shared_ptr<arrow::Schema> local_schema =
        table != nullptr ? table->schema() : nullptr;
    std::vector<std::shared_ptr<arrow::Schema>> schemas;

    GlobalAllGatherv(local_schema, schemas, comm_spec);
    BOOST_LEAF_AUTO(normalized_schema, TypeLoosen(schemas));

    if (table == nullptr) {
      std::shared_ptr<arrow::Table> table_out;
      VY_OK_OR_RAISE(
          vineyard::EmptyTableBuilder::Build(normalized_schema, table_out));
      return table_out;
    } else {
      return CastTableToSchema(table, normalized_schema);
    }
  }

  // Inspired by arrow::compute::Cast
  boost::leaf::result<std::shared_ptr<arrow::Array>> CastIntToDouble(
      const std::shared_ptr<arrow::Array>& in,
      const std::shared_ptr<arrow::DataType>& to_type) {
    LOG(INFO) << in->type()->ToString();
    CHECK_OR_RAISE(in->type()->Equals(arrow::int64()));
    CHECK_OR_RAISE(to_type->Equals(arrow::float64()));
    using in_type = int64_t;
    using out_type = double;
    auto in_data = in->data()->GetValues<in_type>(1);
    std::vector<out_type> out_data(in->length());
    for (int64_t i = 0; i < in->length(); ++i) {
      out_data[i] = static_cast<out_type>(*in_data++);
    }
    arrow::DoubleBuilder builder;
    ARROW_OK_OR_RAISE(builder.AppendValues(out_data));
    std::shared_ptr<arrow::Array> out;
    ARROW_OK_OR_RAISE(builder.Finish(&out));
    ARROW_OK_OR_RAISE(out->ValidateFull());
    return out;
  }

  // Timestamp value are stored as as number of seconds, milliseconds,
  // microseconds or nanoseconds since UNIX epoch.
  // CSV reader can only produce timestamp in seconds.
  boost::leaf::result<std::shared_ptr<arrow::Array>> CastDateToInt(
      const std::shared_ptr<arrow::Array>& in,
      const std::shared_ptr<arrow::DataType>& to_type) {
    CHECK_OR_RAISE(
        in->type()->Equals(arrow::timestamp(arrow::TimeUnit::SECOND)));
    CHECK_OR_RAISE(to_type->Equals(arrow::int64()));
    auto array_data = in->data()->Copy();
    array_data->type = to_type;
    auto out = arrow::MakeArray(array_data);
    ARROW_OK_OR_RAISE(out->ValidateFull());
    return out;
  }

  boost::leaf::result<std::shared_ptr<arrow::Table>> CastTableToSchema(
      const std::shared_ptr<arrow::Table>& table,
      const std::shared_ptr<arrow::Schema>& schema) {
    if (table->schema()->Equals(schema)) {
      return table;
    }
    CHECK_OR_RAISE(table->num_columns() == schema->num_fields());
    std::vector<std::shared_ptr<arrow::ChunkedArray>> new_columns;
    for (int64_t i = 0; i < table->num_columns(); ++i) {
      auto col = table->column(i);
      if (!table->field(i)->type()->Equals(schema->field(i)->type())) {
        auto from_type = table->field(i)->type();
        auto to_type = schema->field(i)->type();
        std::vector<std::shared_ptr<arrow::Array>> chunks;
        for (int64_t j = 0; j < col->num_chunks(); ++j) {
          auto array = col->chunk(j);
          if (from_type->Equals(arrow::int64()) &&
              to_type->Equals(arrow::float64())) {
            BOOST_LEAF_AUTO(new_array, CastIntToDouble(array, to_type));
            chunks.push_back(new_array);
          } else if (from_type->Equals(
                         arrow::timestamp(arrow::TimeUnit::SECOND)) &&
                     to_type->Equals(arrow::int64())) {
            BOOST_LEAF_AUTO(new_array, CastDateToInt(array, to_type));
            chunks.push_back(new_array);
          } else {
            RETURN_GS_ERROR(ErrorCode::kDataTypeError,
                            "Unexpected type: " + to_type->ToString() +
                                "; Origin type: " + from_type->ToString());
          }
          LOG(INFO) << "Cast " << from_type->ToString() << " To "
                    << to_type->ToString();
        }
        auto chunk_array =
            std::make_shared<arrow::ChunkedArray>(chunks, to_type);
        new_columns.push_back(chunk_array);
      } else {
        new_columns.push_back(col);
      }
    }
    return arrow::Table::Make(schema, new_columns);
  }

  std::map<std::string, label_id_t> vertex_label_to_index_;
  std::map<std::string, label_id_t> edge_label_to_index_;
  std::map<std::string, std::set<std::pair<std::string, std::string>>>
      edge_vertex_label_;

  vineyard::Client& client_;
  grape::CommSpec comm_spec_;
  std::vector<std::string> efiles_, vfiles_;

  label_id_t vertex_label_num_, edge_label_num_;
  std::vector<std::shared_ptr<arrow::Table>> partial_v_tables_;
  std::vector<std::vector<std::shared_ptr<arrow::Table>>> partial_e_tables_;
  partitioner_t partitioner_;

  bool directed_;
  basic_loader_t basic_arrow_fragment_loader_;
  std::function<void(vineyard::LocalIOAdaptor*)> io_deleter_ =
      [](vineyard::LocalIOAdaptor* adaptor) {
        VINEYARD_CHECK_OK(adaptor->Close());
        delete adaptor;
      };
};

}  // namespace vineyard

#endif  // MODULES_GRAPH_LOADER_ARROW_FRAGMENT_LOADER_H_
