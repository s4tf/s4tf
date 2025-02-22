/*
 * Copyright 2020 TensorFlow Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef X10_XLA_CLIENT_XRT_COMPUTATION_CLIENT_H_
#define X10_XLA_CLIENT_XRT_COMPUTATION_CLIENT_H_

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "absl/types/optional.h"
#include "tensorflow/compiler/xla/xla_client/cache.h"
#include "tensorflow/compiler/xla/xla_client/computation_client.h"
#include "tensorflow/compiler/xla/xla_client/debug_macros.h"
#include "tensorflow/compiler/xla/xla_client/device.h"
#include "tensorflow/compiler/xla/xla_client/mesh_service.h"
#include "tensorflow/compiler/xla/xla_client/metrics.h"
#include "tensorflow/compiler/xla/xla_client/triggered_task.h"
#include "tensorflow/compiler/xla/xla_client/util.h"
#include "tensorflow/compiler/xla/xla_client/xrt_session.h"
#include "tensorflow/compiler/xla/xla_client/xrt_session_cache.h"
#include "tensorflow/cc/client/client_session.h"
#include "tensorflow/cc/framework/ops.h"
#include "tensorflow/cc/framework/scope.h"
#include "tensorflow/cc/ops/standard_ops.h"
#include "tensorflow/compiler/xla/xla_data.pb.h"
#include "tensorflow/compiler/xrt/cc/ops/xrt_compile_ops.h"
#include "tensorflow/compiler/xrt/cc/ops/xrt_execute_op.h"
#include "tensorflow/compiler/xrt/cc/ops/xrt_state_ops.h"
#include "tensorflow/compiler/xrt/xrt.pb.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/protobuf/tpu/topology.pb.h"

namespace xla {

class XrtComputationClient : public ComputationClient,
                             public ComputationClient::TransferManager {
  struct DeviceHandle {
    std::string device;
    int64_t handle;
  };

  class XrtDevice;

  struct XrtHandle {
    XrtHandle(int64_t handle, std::function<void()> releaser)
        : handle(handle), releaser(std::move(releaser)) {}

    ~XrtHandle() { releaser(); }

    int64_t handle;
    std::function<void()> releaser;
  };

  using XrtHandlePtr = std::shared_ptr<XrtHandle>;

  struct XrtData : public Data {
    XrtData(Device* device, Shape device_shape)
        : Data(device, std::move(device_shape)) {}
    XrtData(XrtDevice* device, Shape device_shape, int64_t handle);

    int64_t get_handle() const { return handle_ptr->handle; }

    OpaqueHandle GetOpaqueHandle() override { return get_handle(); }

    void Assign(const Data& data) override;

    bool HasValue() const override { return handle_ptr != nullptr; }

    XrtHandlePtr handle_ptr;
  };

  struct XrtComputation : public Computation {
    XrtComputation(XrtComputationClient* self, XlaComputation computation,
                   ProgramShape program_shape, std::vector<std::string> devices,
                   int64_t handle, std::string compilation_device)
        : Computation(std::move(computation), std::move(program_shape),
                      std::move(devices)),
          handle_ptr(std::make_shared<XrtHandle>(
              handle, [self, compilation_device = std::move(compilation_device),
                       handle]() {
                self->ReleaseXrtComputation(compilation_device, handle);
              })) {}

    int64_t get_handle() const { return handle_ptr->handle; }

    XrtHandlePtr handle_ptr;
  };

 public:
  struct DeviceId {
    DeviceId() = default;
    DeviceId(const std::string& device_str);

    std::string kind;
    int ordinal = 0;
  };

  struct Worker {
    Worker(std::string name, int task_no)
        : name(std::move(name)), task_no(task_no) {}

    bool operator<(const Worker& rhs) const {
      if (task_no != rhs.task_no) {
        return task_no < rhs.task_no;
      }
      return name.compare(rhs.name) < 0;
    }

    bool operator==(const Worker& rhs) const {
      return task_no == rhs.task_no && name == rhs.name;
    }

    std::string name;
    int task_no;
  };

  struct Options {
    std::string default_device;
    // Maps a S4TF device ID (example, "GPU:0", "TPU:0") to the full
    // coordinates in TF device format
    // (ie, /job:tpu_worker/replica:0/task:0/device:TPU:0), of the worker
    // exposing that device. These devices are all the devices present within
    // the TPU mesh.
    std::map<std::string, std::string> global_device_map;
    // These are the devices that this instance of S4TF is handling. These
    // devices are in the form of "CPU:0", "TPU:3", ... For each of these
    // devices, there is an entry within the global_device_map.
    std::set<std::string> devices;
    // Maps a TPU Worker with an EndPoint.
    std::map<Worker, std::string> workers_map;
  };

  XrtComputationClient(
      Options options,
      std::unique_ptr<tensorflow::tpu::TopologyProto> topology_proto);

  std::vector<Literal> TransferFromServerImpl(
      absl::Span<const DataPtr> handles);

  std::vector<ComputationPtr> Compile(const std::string& device,
                                      const std::vector<std::string>& devices,
                                      std::vector<CompileInstance> instances);

  std::vector<DataPtr> ExecuteComputation(
      const Computation& computation, absl::Span<const DataPtr> arguments,
      const std::string& device, const ExecuteComputationOptions& options);

  std::vector<std::vector<DataPtr>> ExecuteReplicated(
      const Computation& computation,
      const std::vector<std::vector<DataPtr>>& arguments,
      absl::Span<const std::string> devices,
      const ExecuteReplicatedOptions& options);

  std::vector<std::vector<DataPtr>> ExecuteParallel(
      absl::Span<const Computation* const> computations,
      const std::vector<std::vector<DataPtr>>& arguments,
      absl::Span<const std::string> devices,
      const ExecuteParallelOptions& options);

  std::vector<DataPtr> ExecuteChained(absl::Span<const ExecuteChainedOp> ops,
                                      const std::string& device);

  std::vector<std::vector<DataPtr>> DeconstructTuple(
      absl::Span<const DataPtr> tuples);

  std::string GetResourceDomain(const std::string& device) const;

  std::string GetDefaultDevice() const override;

  swift_xla::Device GetDefaultDeviceStruct() const override;

  size_t GetNumDevices() const;

  std::vector<std::string> GetLocalDevices() const;

  void SetRngSeed(size_t seed) override;

  std::map<std::string, Metric> GetMetrics() const override;

  static Worker ParseWorker(const std::string& worker);

  static std::string GetMultiProcessingDevice();

 private:
  // The data structure used for the key in the compilation cache. Compilations
  // handles are valid within given domain (essentially the host+port worker
  // endpoints), so the key must include the domain.
  struct CompilationCacheKey {
    struct Hash {
      size_t operator()(const CompilationCacheKey& entry) const {
        util::PartialHasher<std::string, 4096> hasher;
        hash_t h = util::DataHash(entry.domain.data(), entry.domain.size());
        return util::HashReduce(
            util::HashCombine(h, hasher(entry.serialized_computation)));
      }
    };

    CompilationCacheKey(std::string domain, std::string serialized_computation)
        : domain(std::move(domain)),
          serialized_computation(std::move(serialized_computation)) {}
    CompilationCacheKey() = default;
    CompilationCacheKey(CompilationCacheKey&&) = default;
    CompilationCacheKey& operator=(CompilationCacheKey&&) = default;
    bool operator==(const CompilationCacheKey& rhs) const {
      return domain == rhs.domain &&
             serialized_computation == rhs.serialized_computation;
    }

    std::string domain;
    std::string serialized_computation;
  };

  // When we split a batch operation into per-session batches, we use this data
  // structure to collect the per-session work.
  struct SessionWork {
    tensorflow::ClientSession::FeedType feed_inputs;
    std::vector<tensorflow::Output> outputs_handles;
    std::vector<tensorflow::Operation> operations;
    std::vector<size_t> index_mapping;
  };

  XrtSession* GetSessionForTarget(XrtSessionCache* cache,
                                  const std::string& target,
                                  XrtSessionCache::SessionMap* session_map);
  XrtSession* GetSessionForXrtDevice(XrtSessionCache* cache,
                                     const std::string& xrt_device,
                                     XrtSessionCache::SessionMap* session_map);
  XrtSession* GetSessionForDevice(XrtSessionCache* cache,
                                  const std::string& device,
                                  XrtSessionCache::SessionMap* session_map);

  std::string GetEffectiveDevice(const std::string& device) const;

  const std::string& SwiftDeviceToXrtDevice(const std::string& device) const;

  std::unique_ptr<xrt::XLAComputation> CreateXrtComputation(
      const XlaComputation& computation, absl::Span<const std::string> devices,
      const Shape* output_shape) const;

  tensorflow::Tensor GetArgumentsInputs(absl::Span<const DataPtr> arguments,
                                        const std::string& device);

  std::vector<tensorflow::Output> CreateExecuteOps(
      XrtSessionCache::SessionMap* session_map,
      absl::Span<const Computation* const> computations,
      const std::vector<std::vector<DataPtr>>& arguments, bool explode_tuple,
      absl::Span<const std::string> devices,
      tensorflow::ClientSession::FeedType* feed_inputs);

  std::vector<tensorflow::Output> CreateExecuteOps(
      XrtSessionCache::SessionMap* session_map,
      const XrtComputation& computation,
      const std::vector<std::vector<DataPtr>>& arguments, bool explode_tuple,
      absl::Span<const std::string> devices,
      tensorflow::ClientSession::FeedType* feed_inputs);

  std::vector<std::vector<DataPtr>> RunComputations(
      const XrtSessionCache::SessionMap& session_map,
      const std::vector<tensorflow::Output>& exec_ops,
      absl::Span<const Computation* const> computations,
      absl::Span<const std::string> devices,
      const tensorflow::ClientSession::FeedType& feed_inputs);

  std::vector<DataPtr> TransferToServerInternal(
      XrtDevice* device_ptr, absl::Span<const TensorSource> tensors);

  // Retrieves the worker,worker_host pair for a given S4TF device (ie,
  // TPU:0).
  std::pair<Worker, std::string> GetWorkerForDevice(
      const std::string& device) const;

  // Retrieves the worker,worker_host pair for a given XRT device (ie,
  // /job:tpu_worker/replica:0/task:0/device:TPU:0).
  std::pair<Worker, std::string> GetWorkerForXrtDevice(
      const std::string& xrt_device) const;

  void ReleaseHandles(std::vector<DeviceHandle>* handles,
                      const std::function<const XrtSession::CachedNode&(
                          XrtSession*, const tensorflow::Scope&,
                          const std::string&)>& op_generator,
                      metrics::Metric* timed_metric,
                      metrics::Counter* destroy_counter);

  void ReleaseHandle(int64_t handle, const std::string& device,
                     std::vector<DeviceHandle>* handles);

  void ReleaseXrtData(const std::string& device, int64_t handle);

  void ReleaseXrtComputation(const std::string& compilation_device,
                             int64_t handle);

  // Starts the handle releaser thread (which runs the HandleReleaser() API).
  void StartHandleReleaser();

  // The handler releaser function. Runs in the releaser thread and never
  // returns.
  void HandleReleaser();

  // Retrieves the mesh coordinates of a given XRT device.
  const std::vector<int>& GetDeviceMeshCoords(
      const std::string& xrt_device) const;

  void InitializeDevices(
      std::unique_ptr<tensorflow::tpu::TopologyProto> topology_proto);

  void CreateMeshService(const std::string& address,
                         const tensorflow::tpu::TopologyProto* topology_proto);

  void SetupGpuRuntime();

  std::vector<DataPtr> GetComputationResults(
      const tensorflow::Tensor& xrt_result, const Shape& result_shape,
      const std::string& device);

  void InitSession(XrtSession* session) const;

  // Implement the chained execution using the XRTExecuteChained op support.
  std::vector<DataPtr> ExecuteChainedXrt(absl::Span<const ExecuteChainedOp> ops,
                                         const std::string& device);

  // Implement the chained execution using multiple XRTExecute in many RPC round
  // trips.
  std::vector<DataPtr> ExecuteChainedSplit(
      absl::Span<const ExecuteChainedOp> ops, const std::string& device);

  // Creates an XRT graph with an XRTCompile operation:
  //
  //  XRTCompile(
  //    holders[0]
  //  )
  //
  // With:
  //  holders[0] = XLA Computation place-holder (DT_STRING)
  const XrtSession::CachedNode& GetCompileNode(XrtSession* session,
                                               const tensorflow::Scope& scope,
                                               const std::string& device) const;

  // Creates an XRT graph with an XRTExecute operation:
  //
  //  XRTExecute(
  //    holders[0],
  //    holders[1],
  //    holders[2]
  //  )
  //
  // With:
  //  holders[0] = XLA Computation handle place-holder (DT_INT64)
  //  holders[1] = xrt::XRTExecutionConfig place-holder (DT_STRING)
  //  holders[2] = Inputs for the XRTExecute (DT_INT64[])
  const XrtSession::CachedNode& GetExecuteNode(XrtSession* session,
                                               const tensorflow::Scope& scope,
                                               const std::string& device) const;

  // Creates an XRT graph with an XRTExecute operation:
  //
  //  XRTExecuteChained(
  //    holders[0],
  //    holders[1]
  //  )
  //
  // With:
  //  holders[0] = xrt::XRTChainedExecutePlan place-holder (DT_STRING)
  //  holders[1] = xrt::XRTChainedExecuteConfig place-holder (DT_STRING)
  const XrtSession::CachedNode& GetExecuteChainedNode(
      XrtSession* session, const tensorflow::Scope& scope,
      const std::string& device) const;

  // Creates an XRT graph with an XRTReadLiteral operation:
  //
  //  XRTReadLiteral(
  //    holders[0]
  //  )
  //
  // With:
  //  holders[0] = The handle place-holder to be read (DT_INT64)
  const XrtSession::CachedNode& GetReadNode(XrtSession* session,
                                            const tensorflow::Scope& scope,
                                            const std::string& device) const;

  // Creates an XRTAllocateFromTensor node for creating a device tensor with
  // the given shape and layout:
  //
  //  XRTAllocateFromTensor(
  //    holders[0]
  //  )
  //
  // With:
  //  holders[0] = Tensor place-holder (DT_* - depends on shape type)
  const XrtSession::CachedNode& GetAllocateNode(XrtSession* session,
                                                const tensorflow::Scope& scope,
                                                const std::string& device,
                                                const Shape& shape) const;

  // Creates an XRTReleaseAllocationHandle node:
  //
  //  XRTReleaseAllocationHandle(
  //    holders[0]
  //  )
  //
  // With:
  //  holders[0] = To be released handle place-holder (DT_INT64)
  const XrtSession::CachedNode& GetReleaseAllocationHandleNode(
      XrtSession* session, const tensorflow::Scope& scope,
      const std::string& device) const;

  // Creates an XRTReleaseCompilationHandle node:
  //
  //  XRTReleaseCompilationHandle(
  //    holders[0]
  //  )
  //
  // With:
  //  holders[0] = To be released compilation handle place-holder (DT_INT64)
  const XrtSession::CachedNode& GetReleaseCompileHandleNode(
      XrtSession* session, const tensorflow::Scope& scope,
      const std::string& device) const;

  // Creates an XRTSubTuple node:
  //
  //  XRTSubTuple(
  //    holders[0],
  //    holders[1]
  //  )
  //
  // With:
  //  holders[0] = Tuple handle place-holder (DT_INT64)
  //  holders[1] = Tuple index place-holder (DT_INT32[])
  const XrtSession::CachedNode& GetSubTupleNode(
      XrtSession* session, const tensorflow::Scope& scope,
      const std::string& device) const;

  // Checks the result of a compile operation, and dumps the XLA computation
  // graphs in case of error.
  static void CheckCompileStatus(const Status& status,
                                 const std::vector<CompileInstance>& instances,
                                 const SessionWork& session_work);

  // Converts an XLA data type to a tensorflow data type.
  static tensorflow::DataType XlaTypeToDataType(PrimitiveType dtype);

  static tensorflow::TensorShape MakeEquivalentTensorShape(const Shape& shape);

  // Builds an argument vector usable in a replicated context, out of a single
  // replica argument vector. Essentially turns a [N] into a [1][N].
  static std::vector<std::vector<DataPtr>> BuildParallelArguments(
      absl::Span<const DataPtr> arguments);

  static std::vector<size_t> PartitionTransferToServer(
      absl::Span<const TensorSource> tensors);

  // Extracts the XlaComputation pointers out of Computation ones. Used to be
  // passed to xrt_util::CheckComputationStatus() for its error reporting.
  static std::vector<const XlaComputation*> GetXlaComputations(
      absl::Span<const Computation* const> computations);

  static tensorflow::ConfigProto CreateConfigProto(const Options& options);

  static tensorflow::tpu::TopologyProto InitializeAndFetchTopology(
      const std::string& job, int task_no, const std::string& worker_host_port,
      const tensorflow::ConfigProto& config);

  static std::string GetLocalTarget(const Options& options);

  // Checks whether a local GRPC service is required, and starts it if need it.
  static void MaybeCreateLocalService(const Options& options);

  Options options_;
  std::mutex lock_;
  std::map<std::string, std::vector<int>> device_mesh_coords_;
  std::unique_ptr<XrtSessionCache> session_cache_;
  std::unique_ptr<XrtSessionCache> alloc_session_cache_;
  std::unique_ptr<util::TriggeredTask> triggered_task_;
  util::Cache<CompilationCacheKey, Computation, CompilationCacheKey::Hash>
      compilation_cache_;
  std::atomic<size_t> rng_seed_;
  // Access to the following members must be done while holding lock_.
  // XRT thread safety semantics.
  std::vector<DeviceHandle> released_data_handles_;
  std::vector<DeviceHandle> released_compile_handles_;
  // The mesh service which is used to coordinate all the client hosts which are
  // feeding different TPU devices in a POD (or slice) training.
  std::unique_ptr<service::MeshService> mesh_service_;
};

}  // namespace xla

#endif  // X10_XLA_CLIENT_XRT_COMPUTATION_CLIENT_H_
