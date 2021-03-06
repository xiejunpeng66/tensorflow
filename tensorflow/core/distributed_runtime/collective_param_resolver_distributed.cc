/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "tensorflow/core/distributed_runtime/collective_param_resolver_distributed.h"

#include "absl/strings/escaping.h"
#include "tensorflow/core/distributed_runtime/cancellable_call.h"
#include "tensorflow/core/distributed_runtime/device_resolver_distributed.h"
#include "tensorflow/core/distributed_runtime/worker_cache.h"
#include "tensorflow/core/framework/device_attributes.pb.h"
#include "tensorflow/core/platform/errors.h"
#include "tensorflow/core/protobuf/config.pb.h"
#include "tensorflow/core/util/device_name_utils.h"

namespace tensorflow {
namespace {

class CompleteGroupCall : public CancellableCall {
 public:
  CompleteGroupCall(const CollGroupParams& group,
                    const DeviceAttributes& device,
                    const CollectiveType& collective_type,
                    CancellationManager* cancel_mgr,
                    const string& remote_worker, WorkerCacheInterface* wc)
      : CancellableCall(cancel_mgr, remote_worker, wc) {
    req_.set_group_key(group.group_key);
    req_.set_group_size(group.group_size);
    req_.set_device_type(group.device_type.type_string());
    *req_.mutable_device_attributes() = device;
    req_.set_collective_type(collective_type);
  }
  ~CompleteGroupCall() override {}

  void IssueCall(const StatusCallback& done) override {
    wi_->CompleteGroupAsync(&opts_, &req_, &resp_, done);
  }

  CompleteGroupRequest req_;
  CompleteGroupResponse resp_;
};

class CompleteInstanceCall : public CancellableCall {
 public:
  CompleteInstanceCall(const CollGroupParams& group,
                       const CollInstanceParams& instance,
                       const string& node_name, const string& device_name,
                       bool is_source, CancellationManager* cancel_mgr,
                       const string& remote_worker, WorkerCacheInterface* wc)
      : CancellableCall(cancel_mgr, remote_worker, wc) {
    req_.set_name(node_name);
    req_.set_type(instance.type);
    req_.set_data_type(instance.data_type);
    instance.shape.AsProto(req_.mutable_shape());
    req_.set_group_key(group.group_key);
    req_.set_group_size(group.group_size);
    req_.set_instance_key(instance.instance_key);
    req_.set_device_type(group.device_type.type_string());
    for (int32 offset : instance.impl_details.subdiv_offsets) {
      req_.add_subdiv_offset(offset);
    }
    req_.set_device(device_name);
    req_.set_is_source(is_source);
  }

  ~CompleteInstanceCall() override {}

  void IssueCall(const StatusCallback& done) override {
    wi_->CompleteInstanceAsync(&opts_, &req_, &resp_, done);
  }

  CompleteInstanceRequest req_;
  CompleteInstanceResponse resp_;
};

}  // namespace

CollectiveParamResolverDistributed::CollectiveParamResolverDistributed(
    const ConfigProto& config, const DeviceMgr* dev_mgr,
    DeviceResolverDistributed* dev_resolver, WorkerCacheInterface* worker_cache,
    const string& task_name)
    : CollectiveParamResolverLocal(config, dev_mgr, dev_resolver, task_name),
      worker_cache_(worker_cache),
      group_leader_(task_name == config.experimental().collective_group_leader()
                        ? ""
                        : config.experimental().collective_group_leader()) {
  VLOG(1) << "CompleteParamResolverDistributed ctor task={" << task_name
          << "} config.collective_group_leader={"
          << config.experimental().collective_group_leader() << "}"
          << " config.collective_nccl={"
          << config.experimental().collective_nccl() << "}";
}

void CollectiveParamResolverDistributed::CompleteParamsAsync(
    const DeviceAttributes& device, CollectiveParams* cp,
    CancellationManager* cancel_mgr, const StatusCallback& done) {
  VLOG(1) << "CompleteParams distributed " << device.name() << " for " << cp
          << ": " << cp->ToString();
  CompleteGroupDistributed(device, cp, cancel_mgr,
                           [this, device, cp, cancel_mgr, done](
                               const Status& s, const GroupRec* gr) {
                             if (s.ok()) {
                               CompleteInstanceDistributed(
                                   device.name(), gr, cp, cancel_mgr, done);
                             } else {
                               done(s);
                             }
                           });
}

void CollectiveParamResolverDistributed::CompleteGroupAsync(
    const CompleteGroupRequest* request, CompleteGroupResponse* response,
    CancellationManager* cancel_mgr, const StatusCallback& done) {
  if (!request->has_device_attributes()) {
    done(errors::Internal(
        "CompleteGroupRequest device_attributes is not set. Make sure you're "
        "running the same version of Tensorflow on all workers."));
    return;
  }
  CollectiveParams cp;
  cp.group.group_key = request->group_key();
  cp.group.group_size = request->group_size();
  cp.group.device_type = DeviceType(request->device_type());
  cp.instance.type = CollectiveType(request->collective_type());
  CompleteGroupDistributed(
      request->device_attributes(), &cp, cancel_mgr,
      [response, done](const Status& s, const GroupRec* gr) {
        if (s.ok()) {
          mutex_lock l(gr->mu);
          response->set_group_key(gr->group.group_key);
          response->set_group_size(gr->group.group_size);
          response->set_device_type(gr->group.device_type.type_string());
          response->set_num_tasks(gr->group.num_tasks);
          for (const auto& item : gr->devices) {
            *response->add_device_attributes() = item.second;
          }
          response->set_communicator_key(
              gr->group.runtime_details.communicator_key);
        } else {
          LOG(ERROR) << "Bad status from CompleteGroupDistributed: " << s;
        }
        done(s);
      });
}

void CollectiveParamResolverDistributed::CompleteInstanceAsync(
    const CompleteInstanceRequest* request, CompleteInstanceResponse* response,
    CancellationManager* cancel_mgr, const StatusCallback& done) {
  GroupRec* gr = GetCachedGroup(request->group_key());
  if (gr == nullptr) {
    done(errors::FailedPrecondition(
        "group ", request->group_key(),
        " not found. This normally means the server has restarted"));
    return;
  }
  {
    mutex_lock l(gr->mu);
    if (!gr->status.ok() || gr->devices.size() != gr->group.group_size) {
      done(errors::FailedPrecondition(
          "group ", request->group_key(),
          " failed to resolve. This normally means the server has restarted"));
      return;
    }
  }
  CollectiveParams* cp = new CollectiveParams;
  cp->name = request->name();
  cp->group.group_key = request->group_key();
  cp->group.group_size = request->group_size();
  cp->group.device_type = DeviceType(request->device_type());
  cp->instance.type = CollectiveType(request->type());
  cp->instance.instance_key = request->instance_key();
  cp->instance.data_type = request->data_type();
  cp->instance.shape = TensorShape(request->shape());
  for (int32 offset : request->subdiv_offset()) {
    cp->instance.impl_details.subdiv_offsets.push_back(offset);
  }
  StatusCallback done_and_cleanup = [cp, done](const Status& s) {
    done(s);
    delete cp;
  };
  CompleteInstanceDistributed(
      request->device(), gr, cp, cancel_mgr,
      [this, gr, cp, response, done_and_cleanup](const Status& ci_status) {
        if (ci_status.ok()) {
          // Now source_rank should be known, so
          // retrieve it.
          FindInstanceRec(
              gr, cp,
              [cp, response, done_and_cleanup](const Status& fi_status,
                                               InstanceRec* ir) {
                if (fi_status.ok()) {
                  mutex_lock l(ir->out_mu);
                  ir->WaitForOutMu(l);
                  response->set_instance_key(cp->instance.instance_key);
                  response->set_source_rank(ir->source_rank);
                  done_and_cleanup(fi_status);
                } else {
                  done_and_cleanup(fi_status);
                }
              });
        } else {
          done_and_cleanup(ci_status);
        }
      });
}

CollectiveParamResolverDistributed::GroupRec*
CollectiveParamResolverDistributed::GetCachedGroup(int32 group_key) {
  mutex_lock l(group_mu_);
  auto it = group_table_.find(group_key);
  if (it == group_table_.end()) {
    return nullptr;
  }
  return it->second.get();
}

Status CollectiveParamResolverDistributed::UpdateGroupCache(
    const CompleteGroupResponse& resp) {
  // Build a new record from resp.
  std::unique_ptr<GroupRec> gr(new GroupRec);
  {
    mutex_lock grl(gr->mu);
    gr->group.device_type = DeviceType(resp.device_type());
    gr->group.group_key = resp.group_key();
    gr->group.group_size = resp.group_size();
    gr->group.num_tasks = resp.num_tasks();
    if (resp.device_attributes().empty()) {
      return errors::Internal(
          "CompleteGroupResponse device_attributes is empty. Make sure you're "
          "running the same version of Tensorflow on all workers.");
    }
    if (resp.device_attributes_size() != gr->group.group_size) {
      return errors::Internal(
          "CompleteGroupResponse group_size doesn't match device_name list");
    }
    for (const DeviceAttributes& device : resp.device_attributes()) {
      gr->devices[device.name()] = device;
    }
    gr->group.runtime_details.communicator_key = resp.communicator_key();
  }
  {
    // Group membership should never change. Once a record is in group_table_
    // it never gets removed.
    mutex_lock l(group_mu_);
    auto it = group_table_.find(gr->group.group_key);
    if (it == group_table_.end()) {
      VLOG(2) << "UpdateGroupCache: communicator_key="
              << absl::CEscape(gr->group.runtime_details.communicator_key);
      group_table_[gr->group.group_key] = std::move(gr);
    } else {
      auto& previous_gr = group_table_[gr->group.group_key];
      if (previous_gr->group.runtime_details.communicator_key !=
          gr->group.runtime_details.communicator_key) {
        return errors::Internal(
            "UpdateGroupCache: CompleteGroupResponse for group ",
            gr->group.group_key, " gives communicator_key=",
            absl::CEscape(gr->group.runtime_details.communicator_key),
            " but cache already holds communicator_key=",
            absl::CEscape(previous_gr->group.runtime_details.communicator_key));
      }
    }
  }
  return Status::OK();
}

void CollectiveParamResolverDistributed::CompleteGroupDistributed(
    const DeviceAttributes& device, CollectiveParams* cp,
    CancellationManager* cancel_mgr, const GroupRecCallback& done) {
  VLOG(1) << "CompleteGroupDistributed group_key=" << cp->group.group_key
          << " dev: " << device.name()
          << " is_leader=" << (group_leader_.empty());
  if (group_leader_.empty()) {
    // This is the group leader, so resolution is local.
    return CompleteGroupLocal(device, cp, done);
  } else if (GetCachedGroup(cp->group.group_key) == nullptr) {
    // Need to update Group cache from the leader.
    CompleteGroupCall* call =
        new CompleteGroupCall(cp->group, device, cp->instance.type, cancel_mgr,
                              group_leader_, worker_cache_);
    call->Start([this, device, cp, call, done](const Status& s) {
      if (s.ok()) {
        Status status = UpdateGroupCache(call->resp_);
        if (status.ok()) {
          CompleteGroupLocal(device, cp, done);
        } else {
          done(status, nullptr);
        }
      } else {
        done(s, nullptr);
      }
      delete call;
    });
    return;
  } else {
    return CompleteGroupLocal(device, cp, done);
  }
}

bool CollectiveParamResolverDistributed::InstanceIsCached(int32 group_key,
                                                          int32 instance_key) {
  mutex_lock l(instance_mu_);
  auto group_it = instance_table_.find(group_key);
  if (group_it == instance_table_.end()) {
    return false;
  }
  auto instance_it = group_it->second.find(instance_key);
  return instance_it != group_it->second.end();
}

void CollectiveParamResolverDistributed::UpdateInstanceCache(
    const GroupRec* gr, CollectiveParams* cp,
    const CompleteInstanceResponse& resp, const StatusCallback& done) {
  using InstanceRecPointer = InstanceRec*;
  InstanceRecPointer* irp = new InstanceRecPointer(nullptr);
  int32 source_rank = resp.source_rank();

  auto continue_with_ir = [cp, irp, source_rank, done](const Status& s) {
    if (!s.ok()) {
      done(s);
      delete irp;
      return;
    }
    Status status;
    InstanceRec* ir = *irp;
    do {
      mutex_lock l(ir->out_mu);
      ir->WaitForOutMu(l);
      if (ir->source_rank != source_rank) {
        if (ir->source_rank >= 0) {
          ir->status = errors::Internal(
              "UpdateInstanceCache: CompleteInstanceResponse for instance ",
              cp->instance.instance_key, " gives source_rank=", source_rank,
              " but cache already holds value=", ir->source_rank);
          status = ir->status;
          break;
        }
        ir->source_rank = source_rank;
      }
      if (ir->known_count < cp->group.group_size) {
        ir->known_count = cp->group.group_size;
        const int ir_known_size = ir->known.size();
        if (ir_known_size != cp->group.group_size) {
          ir->status = errors::Internal(
              "UpdateInstanceCache:: CompleteInstanceResponse for instance ",
              cp->instance.instance_key, " has known.size()=", ir->known.size(),
              " < group_size=", cp->group.group_size);
          status = ir->status;
          break;
        }
        for (int i = 0; i < ir_known_size; ++i) {
          ir->known[i] = true;
        }
      }
      status = ir->status;
    } while (false);
    // Callback outside of lock.
    done(status);
    delete irp;
  };

  FindInstanceRec(gr, cp,
                  [irp, continue_with_ir](const Status s, InstanceRec* irec) {
                    *irp = irec;
                    continue_with_ir(s);
                  });
}

void CollectiveParamResolverDistributed::CompleteInstanceDistributed(
    const string& device, const GroupRec* gr, CollectiveParams* cp,
    CancellationManager* cancel_mgr, const StatusCallback& done) {
  if (group_leader_.empty()) {
    // This is the group leader so resolution is local.
    return CompleteInstanceLocal(device, gr, cp, cp->is_source, done);
  } else if (InstanceIsCached(gr->group.group_key, cp->instance.instance_key)) {
    return CompleteInstanceLocal(device, gr, cp, cp->is_source, done);
  } else {
    CompleteInstanceCall* call = new CompleteInstanceCall(
        cp->group, cp->instance, cp->name, device, cp->is_source, cancel_mgr,
        group_leader_, worker_cache_);
    call->Start([this, device, gr, cp, call, done](const Status& s) {
      if (s.ok()) {
        UpdateInstanceCache(
            gr, cp, call->resp_, [this, device, gr, cp, done](const Status& s) {
              if (!s.ok()) {
                done(s);
              } else {
                CompleteInstanceLocal(device, gr, cp, cp->is_source, done);
              }
            });
      } else {
        done(s);
      }
      delete call;
    });
    return;
  }
}

}  // namespace tensorflow
