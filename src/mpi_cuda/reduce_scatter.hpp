////////////////////////////////////////////////////////////////////////////////
// Copyright (c) 2018, Lawrence Livermore National Security, LLC.  Produced at the
// Lawrence Livermore National Laboratory in collaboration with University of
// Illinois Urbana-Champaign.
//
// Written by the LBANN Research Team (N. Dryden, N. Maruyama, et al.) listed in
// the CONTRIBUTORS file. <lbann-dev@llnl.gov>
//
// LLNL-CODE-756777.
// All rights reserved.
//
// This file is part of Aluminum GPU-aware Communication Library. For details, see
// http://software.llnl.gov/Aluminum or https://github.com/LLNL/Aluminum.
//
// Licensed under the Apache License, Version 2.0 (the "Licensee"); you
// may not use this file except in compliance with the License.  You may
// obtain a copy of the License at:
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
// implied. See the License for the specific language governing
// permissions and limitations under the license.
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "cuda.hpp"
#include "cuda_kernels.hpp"
#include "mpi_cuda/communicator.hpp"
#include "progress.hpp"

namespace Al {
namespace internal {
namespace mpi_cuda {

template <typename T>
class ReduceScatterAlState : public AlState {
public:
  ReduceScatterAlState(const T* sendbuf, T* recvbuf, size_t count,
                       ReductionOperator op, MPICUDACommunicator& comm,
                       cudaStream_t stream) :
    AlState(nullptr),
    count_(count),
    host_mem_(get_pinned_memory<T>(comm.size()*count_)),
    op_(mpi::ReductionOperator2MPI_Op(op)),
    comm_(comm.get_comm()) {

    // Transfer data from device to host and use an event to determine when it
    // completes.
    AL_CHECK_CUDA(cudaMemcpyAsync(host_mem_, sendbuf, sizeof(T)*count*comm.size(),
                                  cudaMemcpyDeviceToHost, stream));
    d2h_event_.record(stream);

    // Have the device wait on the host.
    gpuwait_.wait(stream);

    // Transfer completed buffer back to device.
    AL_CHECK_CUDA(cudaMemcpyAsync(recvbuf, host_mem_, sizeof(T)*count,
                                  cudaMemcpyHostToDevice, stream));
    h2d_event_.record(stream);
  }

  ~ReduceScatterAlState() override {
    release_pinned_memory(host_mem_);
  }

  bool step() override {
    if (!rs_started_) {
      // Wait for memory to get to the host.
      if (d2h_event_.query()) {
        MPI_Ireduce_scatter_block(MPI_IN_PLACE, host_mem_, count_,
                                  mpi::TypeMap<T>(), op_, comm_, &req_);
        rs_started_ = true;
      } else {
        return false;
      }
    }
    if (!rs_done_) {
      // Wait for the RS to complete.
      int flag;
      MPI_Test(&req_, &flag, MPI_STATUS_IGNORE);
      if (flag) {
        rs_done_ = true;
        gpuwait_.signal();
      } else {
        return false;
      }
    }
    // Wait for host-to-device memcopy; cleanup
    if (h2d_event_.query()) {
      return true;
    }
    return false;
  }

  bool needs_completion() const override { return false; }

private:
  size_t count_;
  T* host_mem_;
  cuda::GPUWait gpuwait_;
  cuda::FastEvent d2h_event_, h2d_event_;
  MPI_Op op_;
  MPI_Comm comm_;
  MPI_Request req_ = MPI_REQUEST_NULL;
  bool rs_started_ = false;
  bool rs_done_ = false;
};

}  // namespace mpi_cuda
}  // namespace internal
}  // namespace Al
