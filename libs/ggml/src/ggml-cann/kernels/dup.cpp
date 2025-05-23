// Copyright 2024-2025 PowerServe Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "kernel_operator.h"

#include <cmath>

using namespace AscendC;

#define BUFFER_NUM 2

template <typename SRC_T, typename DST_T>
class DupByRows {
   public:
    __aicore__ inline DupByRows() {}
    __aicore__ inline void init(GM_ADDR src, GM_ADDR dst, int64_t *input_ne_ub,
                                size_t *input_nb_ub) {
        /* Dup by rows when src is contigous on first dimension and dst is
        contiguous, each kernel process one row.
        */

        // Input has four dims.
        int64_t op_block_num = GetBlockNum();
        int64_t op_block_idx = GetBlockIdx();

        // param
        num_rows = input_ne_ub[1] * input_ne_ub[2] * input_ne_ub[3];
        num_elem = input_ne_ub[0];

        // index for (ne[1], ne[2], ne[3]): (idx_ne1, idx_ne2, idx_ne3)
        idx_ne3 = op_block_idx / (input_ne_ub[1] * input_ne_ub[2]);
        idx_ne2 = (op_block_idx - idx_ne3 * (input_ne_ub[1] * input_ne_ub[2]))
                  / (input_ne_ub[1]);
        idx_ne1 = op_block_idx - idx_ne3 * (input_ne_ub[1] * input_ne_ub[2])
                - idx_ne2 * input_ne_ub[1];

        // src may not contiguous in dim [1,2,3], so stride decited by ne&nb
        src_stride = input_nb_ub[3] * idx_ne3 + input_nb_ub[2] * idx_ne2
                     + input_nb_ub[1] * idx_ne1;

        // dst is contiguous
        dst_stride = op_block_idx * (input_ne_ub[0] * sizeof(DST_T));

        src_gm.SetGlobalBuffer(reinterpret_cast<__gm__ SRC_T *>(src +
                                                                src_stride));
        dst_gm.SetGlobalBuffer(reinterpret_cast<__gm__ DST_T *>(dst +
                                                                dst_stride));

        pipe.InitBuffer(src_queue, BUFFER_NUM, (sizeof(SRC_T) * num_elem +
                                                32 - 1) / 32 * 32);
        pipe.InitBuffer(dst_queue, BUFFER_NUM, (sizeof(DST_T) * num_elem +
                                                32 - 1) / 32 * 32);
    }

    __aicore__ inline void copy_in() {
        LocalTensor<SRC_T> src_local = src_queue.AllocTensor<SRC_T>();

        DataCopyExtParams dataCopyParams;
        dataCopyParams.blockCount = 1;
        dataCopyParams.blockLen = num_elem * sizeof(SRC_T);
        DataCopyPadExtParams<SRC_T> padParams;
        DataCopyPad(src_local, src_gm, dataCopyParams, padParams);

        src_queue.EnQue(src_local);
    }

    __aicore__ inline void copy_out() {
        LocalTensor<DST_T> dst_local = dst_queue.DeQue<DST_T>();

        DataCopyExtParams dataCopyParams;
        dataCopyParams.blockCount = 1;
        dataCopyParams.blockLen = num_elem * sizeof(DST_T);
        DataCopyPad(dst_gm, dst_local, dataCopyParams);

        dst_queue.FreeTensor(dst_local);
    }

    __aicore__ inline void dup() {
        // main process, copy one row data from src to dst.
        copy_in();

        LocalTensor<SRC_T> src_local = src_queue.DeQue<SRC_T>();
        LocalTensor<DST_T> dst_local = dst_queue.AllocTensor<DST_T>();

        int32_t BLOCK_NUM = 32 / sizeof(DST_T);
        DataCopy(dst_local, src_local, (num_elem + BLOCK_NUM - 1)
                                        / BLOCK_NUM * BLOCK_NUM);
        dst_queue.EnQue<DST_T>(dst_local);

        src_queue.FreeTensor(src_local);
        copy_out();
    }

    __aicore__ inline void dup_with_cast() {
        // main process, copy one row data from src to dst.
        // cast dtype from src to dst.
        copy_in();

        LocalTensor<SRC_T> src_local = src_queue.DeQue<SRC_T>();
        LocalTensor<DST_T> dst_local = dst_queue.AllocTensor<DST_T>();

        Cast(dst_local, src_local, RoundMode::CAST_NONE, num_elem);
        dst_queue.EnQue<DST_T>(dst_local);

        src_queue.FreeTensor(src_local);
        copy_out();
    }

   private:

    TPipe pipe;
    GlobalTensor<SRC_T> src_gm;
    GlobalTensor<DST_T> dst_gm;

    int64_t num_rows;
    int64_t num_elem;
    int64_t idx_ne3;
    int64_t idx_ne2;
    int64_t idx_ne1;
    int64_t src_stride;
    int64_t dst_stride;

    TQue<QuePosition::VECIN, BUFFER_NUM> src_queue;
    TQue<QuePosition::VECOUT, BUFFER_NUM> dst_queue;
};

template <typename T>
__aicore__ inline void copy_to_ub(GM_ADDR gm, T *ub, size_t size) {
    auto gm_ptr = (__gm__ uint8_t *)gm;
    auto ub_ptr = (uint8_t *)(ub);
    for (int32_t i = 0; i < size; ++i, ++ub_ptr, ++gm_ptr) {
        *ub_ptr = *gm_ptr;
    }
}

extern "C" __global__ __aicore__ void ascendc_dup_by_rows_fp16(
                                                        GM_ADDR src_gm,
                                                        GM_ADDR dst_gm,
                                                        GM_ADDR input_ne_gm,
                                                        GM_ADDR input_nb_gm,
                                                        GM_ADDR output_ne_gm,
                                                        GM_ADDR output_nb_gm) {

    int64_t input_ne_ub[4];
    size_t input_nb_ub[4];
    int64_t output_ne_ub[4];
    size_t output_nb_ub[4];

    copy_to_ub(input_ne_gm, input_ne_ub, 32);
    copy_to_ub(input_nb_gm, input_nb_ub, 32);
    copy_to_ub(output_ne_gm, output_ne_ub, 32);
    copy_to_ub(output_nb_gm, output_nb_ub, 32);

    DupByRows<half, half> op;
    op.init(src_gm, dst_gm, input_ne_ub, input_nb_ub);
    op.dup();
}

extern "C" __global__ __aicore__ void ascendc_dup_by_rows_fp32(
                                                        GM_ADDR src_gm,
                                                        GM_ADDR dst_gm,
                                                        GM_ADDR input_ne_gm,
                                                        GM_ADDR input_nb_gm,
                                                        GM_ADDR output_ne_gm,
                                                        GM_ADDR output_nb_gm) {
    int64_t input_ne_ub[4];
    size_t input_nb_ub[4];
    int64_t output_ne_ub[4];
    size_t output_nb_ub[4];

    copy_to_ub(input_ne_gm, input_ne_ub, 32);
    copy_to_ub(input_nb_gm, input_nb_ub, 32);
    copy_to_ub(output_ne_gm, output_ne_ub, 32);
    copy_to_ub(output_nb_gm, output_nb_ub, 32);

    DupByRows<float_t, float_t> op;
    op.init(src_gm, dst_gm, input_ne_ub, input_nb_ub);
    op.dup();
}

extern "C" __global__ __aicore__ void ascendc_dup_by_rows_fp32_to_fp16(
                                                        GM_ADDR src_gm,
                                                        GM_ADDR dst_gm,
                                                        GM_ADDR input_ne_gm,
                                                        GM_ADDR input_nb_gm,
                                                        GM_ADDR output_ne_gm,
                                                        GM_ADDR output_nb_gm) {

    int64_t input_ne_ub[4];
    size_t input_nb_ub[4];
    int64_t output_ne_ub[4];
    size_t output_nb_ub[4];

    copy_to_ub(input_ne_gm, input_ne_ub, 32);
    copy_to_ub(input_nb_gm, input_nb_ub, 32);
    copy_to_ub(output_ne_gm, output_ne_ub, 32);
    copy_to_ub(output_nb_gm, output_nb_ub, 32);

    DupByRows<float_t, half> op;
    op.init(src_gm, dst_gm, input_ne_ub, input_nb_ub);
    op.dup_with_cast();
}

extern "C" __global__ __aicore__ void ascendc_dup_by_rows_fp16_to_fp32(
                                                        GM_ADDR src_gm,
                                                        GM_ADDR dst_gm,
                                                        GM_ADDR input_ne_gm,
                                                        GM_ADDR input_nb_gm,
                                                        GM_ADDR output_ne_gm,
                                                        GM_ADDR output_nb_gm) {

    // copy params from gm to ub.
    int64_t input_ne_ub[4];
    size_t input_nb_ub[4];
    int64_t output_ne_ub[4];
    size_t output_nb_ub[4];

    copy_to_ub(input_ne_gm, input_ne_ub, 32);
    copy_to_ub(input_nb_gm, input_nb_ub, 32);
    copy_to_ub(output_ne_gm, output_ne_ub, 32);
    copy_to_ub(output_nb_gm, output_nb_ub, 32);

    DupByRows<half, float_t> op;
    op.init(src_gm, dst_gm, input_ne_ub, input_nb_ub);
    op.dup_with_cast();
}
