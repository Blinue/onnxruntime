/*++

Copyright (c) Microsoft Corporation. All rights reserved.

Licensed under the MIT License.

Module Name:

    qnbitgemm.h

Abstract:

    This module includes kernel function prototypes and helper functions for
    implementing SQNBitGemm.

    SQNBitGemm is a matrix/matrix multiplication, A*B, where A is a float
    matrix and B is a n-bit quantized integer matrix. B is block quantized,
    meaning values of B are divided into blocks and each block has its own
    scale and optional zero point.

--*/

#pragma once

#include "mlas_qnbit.h"
#include "mlasi.h"

constexpr MLAS_FORCEINLINE size_t
MlasQNBitQuantBBlkSumAlignment()
{
    // 16 floats. this alignment is required by GemmFloatKernel
    return 16 * sizeof(float);
}

constexpr MLAS_FORCEINLINE size_t
MlasQNBitBlkDataSizeInBytes(size_t BlkBitWidth, size_t BlkLen)
{
    return BlkLen * BlkBitWidth / 8;
}

MLAS_FORCEINLINE void*
MlasAlignAddress(void* addr, const size_t alignment)
{
    const uintptr_t QuantBBlkSumAddr = reinterpret_cast<uintptr_t>(addr);
    addr = (void*)((QuantBBlkSumAddr + alignment - 1) & (~(alignment - 1)));
    return addr;
}

template <typename T, int BlkBitWidth>
struct PackedQuantBDataStruct {
    PackedQuantBDataStruct(void* PackedQuantBWorkspace, size_t N, size_t BlockCountK, size_t BlkLen)
        : QuantBWorkspace_(PackedQuantBWorkspace), N_(N), BlockCountK_(BlockCountK), BlkLen_(BlkLen)
    {
        const size_t PackedQuantBDataSize = N * BlockCountK * MlasQNBitBlkDataSizeInBytes(BlkBitWidth, BlkLen);
        size_t BlkSumSize = MlasDivRoundup(N, 16) * BlockCountK * 16 * sizeof(T);
#if defined(MLAS_TARGET_AMD64_IX86)
        // _mm256_load_si256 requires alignment on a 32-byte boundary
        PackedQuantBData = (std::byte*)MlasAlignAddress(PackedQuantBWorkspace, 32);
#else
        PackedQuantBData = (std::byte*)PackedQuantBWorkspace;
#endif
        QuantBBlkSum = (T*)(PackedQuantBData + PackedQuantBDataSize);
        QuantBBlkSum = (T*)MlasAlignAddress(QuantBBlkSum, MlasQNBitQuantBBlkSumAlignment());
        PackedQuantBScale = (T*)((std::byte*)QuantBBlkSum + BlkSumSize);
    }
    std::byte* PackedQuantBData;
    T* PackedQuantBScale;
    T* QuantBBlkSum;

    void* QuantBWorkspace_;
    size_t N_, BlockCountK_, BlkLen_;
};

template <size_t BlkBitWidth>
constexpr MLAS_FORCEINLINE size_t
MlasQNBitZeroPointsForBlksSizeInBytes(size_t BlkCount)
{
    if constexpr (BlkBitWidth <= 4) {
        return MlasDivRoundup(BlkCount, 2);  // 2 blocks per byte
    } else {
        return BlkCount;
    }
}

//
// Kernel dispatch structure.
//

struct MLAS_QNBIT_GEMM_DISPATCH {
    //
    // Quantized B data packing function prototypes.
    //

    /** Gets size of packed quantized B data containing 4-bit integers. See MlasQNBitGemmPackQuantBDataSize(). */
    typedef size_t(Q4BitGemmPackQuantBDataSize_Fn)(
        size_t N,
        size_t K,
        size_t BlkLen,
        bool HasZeroPoint,
        MLAS_QNBIT_GEMM_COMPUTE_TYPE ComputeType
    );

    Q4BitGemmPackQuantBDataSize_Fn* Q4BitGemmPackQuantBDataSize = nullptr;

    /** Gets size of packed quantized B data containing 8-bit integers. See MlasQNBitGemmPackQuantBDataSize(). */
    typedef size_t(Q8BitGemmPackQuantBDataSize_Fn)(
        size_t N,
        size_t K,
        size_t BlkLen,
        bool HasZeroPoint,
        MLAS_QNBIT_GEMM_COMPUTE_TYPE ComputeType
    );

    Q8BitGemmPackQuantBDataSize_Fn* Q8BitGemmPackQuantBDataSize = nullptr;

    /** Packs quantized B data containing 4-bit integers. See MlasQNBitGemmPackQuantBData(). */
    typedef void(Q4BitGemmPackQuantBData_Fn)(
        size_t N,
        size_t K,
        size_t BlkLen,
        MLAS_QNBIT_GEMM_COMPUTE_TYPE ComputeType,
        const std::byte* QuantBDataBegin,
        std::byte* PackedQuantBDataBegin,
        MLAS_THREADPOOL* ThreadPool
    );

    Q4BitGemmPackQuantBData_Fn* SQ4BitGemmPackQuantBData = nullptr;
    Q4BitGemmPackQuantBData_Fn* HQ4BitGemmPackQuantBData = nullptr;

    typedef void(SQ4BitGemmPackQuantBDataAndSumBlk_Fn)(
        size_t N,
        size_t K,
        size_t BlkLen,
        MLAS_QNBIT_GEMM_COMPUTE_TYPE ComputeType,
        const std::byte* QuantBDataBegin,
        const float* QuantBScaleBegin,
        bool HasZeroPoint,
        const std::byte* QuantBZPBegin,
        PackedQuantBDataStruct<float, 4>& PackedQuantB,
        MLAS_THREADPOOL* ThreadPool
    );

    SQ4BitGemmPackQuantBDataAndSumBlk_Fn* SQ4BitGemmPackQuantBDataAndBlkSum = nullptr;

    typedef void(SQ8BitGemmPackQuantBDataAndSumBlk_Fn)(
        size_t N,
        size_t K,
        size_t BlkLen,
        MLAS_QNBIT_GEMM_COMPUTE_TYPE ComputeType,
        const std::byte* QuantBDataBegin,
        const float* QuantBScaleBegin,
        bool HasZeroPoint,
        const std::byte* QuantBZPBegin,
        PackedQuantBDataStruct<float, 8>& PackedQuantB,
        MLAS_THREADPOOL* ThreadPool
    );

    SQ8BitGemmPackQuantBDataAndSumBlk_Fn* SQ8BitGemmPackQuantBDataAndBlkSum = nullptr;

    //
    // Workspace size calculation function prototypes.
    //

    /**
     * @brief Gets the required size in bytes of the per-GEMM intermediate workspace.
     *        Returns a size of zero if no intermediate workspace is needed.
     *
     * @param[in]   M               row size of matrix A and C
     * @param[in]   N               column size of matrix B and C
     * @param[in]   K               column size of matrix A and row size of matrix B
     * @param[in]   BlkLen          number of quantized values per block
     * @param[in]   HasZeroPoint    whether zero points are provided
     * @param[in]   ComputeType     GEMM compute type (e.g., multiplying float or int8 values)
     */
    typedef size_t(QNBitGemmPerGemmWorkspaceSize_Fn)(
        size_t M,
        size_t N,
        size_t K,
        size_t BlkLen,
        bool HasZeroPoint,
        MLAS_QNBIT_GEMM_COMPUTE_TYPE ComputeType
    );

    QNBitGemmPerGemmWorkspaceSize_Fn* QNBitGemmPerGemmWorkspaceSize = nullptr;

    /**
     * @brief Gets the required byte alignment of the per-GEMM intermediate workspace.
     *
     * @param[in]   BlkLen          number of quantized values per block
     * @param[in]   ComputeType     GEMM compute type (e.g., multiplying float or int8 values)
     */
    typedef size_t(QNBitGemmPerGemmWorkspaceAlignment_Fn)(
        size_t BlkLen,
        MLAS_QNBIT_GEMM_COMPUTE_TYPE ComputeType
    );

    QNBitGemmPerGemmWorkspaceAlignment_Fn* QNBitGemmPerGemmWorkspaceAlignment = nullptr;

    //
    // SQNBIT_CompFp32 kernel function prototypes.
    //

    /**
     * @brief Multiply float matrix A with quantized 4-bit integer matrix B.
     *        B is block quantized and column major.
     *        This kernel handles the special case where M, the number of rows of A and C, is 1.
     *
     * @param       BlkLen              Number of values in a block.
     * @param       A                   Supplies the A matrix.
     * @param       QuantBData          Supplies the quantized B matrix block data.
     * @param       QuantBScale         Supplies the quantized B matrix block scale values.
     * @param       QuantBZeroPoint     Supplies the quantized B matrix block zero point values. Optional.
     * @param[out]  C                   Supplies the output C matrix.
     * @param       CountN              Number of columns of B and C.
     * @param       CountK              Number of columns of A and rows of B.
     * @param       BlockStrideQuantB   Number of blocks between adjacent columns of the quantized B matrix.
     * @param       Bias                Bias vector of length N.
     */
    typedef void(SQ4BitGemmM1Kernel_CompFp32_Fn)(
        size_t BlkLen,
        const float* A,
        const std::byte* QuantBData,
        const float* QuantBScale,
        const std::byte* QuantBZeroPoint,
        float* C,
        size_t CountN,
        size_t CountK,
        size_t BlockStrideQuantB,
        const float* Bias
    );

    SQ4BitGemmM1Kernel_CompFp32_Fn* SQ4BitGemmM1Kernel_CompFp32 = nullptr;

    /**
     * @brief Dequantize B into the format expected by the Sgemm kernel.
     *        B is a quantized 4-bit integer matrix that is block quantized and column major.
     *        This is equivalent to dequantizing B and then running MlasSgemmCopyPackB.
     *
     * @param       BlkLen              Number of values in a block.
     * @param[out]  FpData              Supplies the output buffer for the dequantized B float data.
     *                                  It should have enough space for
     *                                      (CountN + 16 - 1) / 16 * 16 * (CountK + BlkLen - 1) / BlkLen * BlkLen
     *                                  elements. Only the first (CountN + 16 - 1) / 16 * 16 * CountK elements are
     *                                  useful, but the kernel implementation can be simplified with the extra space.
     * @param       QuantBData          Supplies the quantized B matrix block data.
     * @param       QuantBScale         Supplies the quantized B matrix block scale values.
     * @param       QuantBZeroPoint     Supplies the quantized B matrix block zero point values. Optional.
     * @param       CountN              Number of columns of B.
     * @param       CountK              Number of rows of B.
     * @param       BlockStrideQuantB   Number of blocks between adjacent columns of the quantized B matrix.
     */
    typedef void(Q4BitBlkDequantBForSgemm_CompFp32_Fn)(
        size_t BlkLen,
        float* FpData,
        const std::byte* QuantBData,
        const float* QuantBScale,
        const std::byte* QuantBZeroPoint,
        size_t CountN,
        size_t CountK,
        size_t BlockStrideQuantB
    );

    Q4BitBlkDequantBForSgemm_CompFp32_Fn* SQ4BitBlkDequantBForSgemm_CompFp32 = nullptr;

    /**
     * @brief Dequantize B into the format expected by the Sgemm kernel.
     *        B is a quantized 4-bit integer matrix that is block quantized and column major.
     *        This is equivalent to dequantizing B and then running MlasSgemmCopyPackB.
     *
     * @param       BlkLen              Number of values in a block.
     * @param[out]  FpData              Supplies the output buffer for the dequantized B float data.
     *                                  It should have enough space for
     *                                      (CountN + 16 - 1) / 16 * 16 * (CountK + BlkLen - 1) / BlkLen * BlkLen
     *                                  elements. Only the first (CountN + 16 - 1) / 16 * 16 * CountK elements are
     *                                  useful, but the kernel implementation can be simplified with the extra space.
     * @param       QuantBData          Supplies the quantized B matrix block data.
     * @param       QuantBScale         Supplies the quantized B matrix block scale values.
     * @param       QuantBZeroPoint     Supplies the quantized B matrix block zero point values. Optional.
     * @param       CountN              Number of columns of B.
     * @param       CountK              Number of rows of B.
     * @param       BlockStrideQuantB   Number of blocks between adjacent columns of the quantized B matrix.
     */
    typedef void(Q4BitBlkDequantBForSgemm_CompFp16_Fn)(
        size_t BlkLen,
        MLAS_FP16* FpData,
        const std::byte* QuantBData,
        const MLAS_FP16* QuantBScale,
        const std::byte* QuantBZeroPoint,
        size_t CountN,
        size_t CountK,
        size_t BlockStrideQuantB
    );

    Q4BitBlkDequantBForSgemm_CompFp16_Fn* HQ4BitBlkDequantBForHgemm_CompFp16 = nullptr;

    //
    // SQNBIT_CompInt8 kernel function prototypes.
    //

    /**
     * @brief Multiply quantized 8-bit integer matrix A with quantized 4-bit integer matrix B.
     *        A and B are block quantized and B is column major.
     *        A should be packed using QuantizeA_Packed_CompInt8.
     *
     * @param       BlkLen              Number of values in a block.
     * @param       QuantA              Supplies the quantized A matrix.
                                        Binary data containing block quantized int8 data and scale values.
     * @param       PackedQuantBData    Supplies the packed quantized B matrix data.
     * @param[out]  C                   Supplies the output C matrix.
     * @param       RangeStartM         Start of M range.
     * @param       RangeCountM         Number of rows of A and C.
     * @param       RangeStartN         Start of N range.
     * @param       RangeCountN         Number of columns of B and C.
     * @param       CountK              Number of columns of A and rows of B.
     * @param       ldc                 Number of elements between adjacent rows of C.
     */
    typedef void(SQ4BitGemmKernel_Packed_CompInt8_Fn)(
        size_t BlkLen,
        const std::byte* QuantA,
        const std::byte* PackedQuantBData,
        float* C,
        const size_t RangeStartM,
        const size_t RangeCountM,
        const size_t RangeStartN,
        const size_t RangeCountN,
        size_t CountK,
        size_t ldc,
        const float* Bias
    );

    SQ4BitGemmKernel_Packed_CompInt8_Fn* SQ4BitGemmKernel_Packed_CompInt8 = nullptr;

    /**
     * @brief Multiply quantized 8-bit integer matrix A with quantized 4-bit integer matrix B.
     *        A and B are block quantized and B is column major.
     *
     * @param       BlkLen              Number of values in a block.
     * @param       QuantA              Supplies the quantized A matrix.
                                        Binary data containing block quantized int8 data and scale values.
     * @param       QuantBData          Supplies the quantized B matrix block data.
     * @param       QuantBScale         Supplies the quantized B matrix block scale values.
     * @param       QuantBZeroPoint     Supplies the quantized B matrix block zero point values. Optional.
     * @param[out]  C                   Supplies the output C matrix.
     * @param       CountN              Number of columns of B and C.
     * @param       CountK              Number of columns of A and rows of B.
     * @param       BlockCountK         Number of blocks between adjacent columns of the quantized B matrix.
     * @param       Bias                Bias vector of length N.
     * @param       ldc                 Number of elements between adjacent rows of C..
     * @param       ABlockSum           Supplies the blksum of A.
     * @param       QuantBBlkSum        Supplies the blksum of B.
     */
    typedef size_t(SQ4BitGemmKernel_BlkSum_CompInt8_Fn)(
        size_t BlkLen,
        const std::byte* QuantA,
        const float* QuantAScale,
        const std::byte* QuantBData,
        const float* QuantBScale,
        const std::byte* QuantBZeroPoint,
        float* C,
        size_t CountM,
        size_t CountN,
        size_t CountK,
        size_t BlockCountK,
        const float* Bias,
        size_t ldc,
        const float* ABlockSum,
        const float* QuantBBlkSum
    );

    SQ4BitGemmKernel_BlkSum_CompInt8_Fn* SQ4BitGemmKernel_BlkSum_CompInt8 = nullptr;

    /**
     * @brief Multiply quantized 8-bit integer matrix A with quantized 8-bit integer matrix B.
     *        A and B are block quantized and B is column major.
     *
     * @param       BlkLen              Number of values in a block.
     * @param       QuantA              Supplies the quantized A matrix.
                                        Binary data containing block quantized int8 data and scale values.
     * @param       QuantBData          Supplies the quantized B matrix block data.
     * @param       QuantBScale         Supplies the quantized B matrix block scale values.
     * @param       QuantBZeroPoint     Supplies the quantized B matrix block zero point values. Optional.
     * @param[out]  C                   Supplies the output C matrix.
     * @param       CountN              Number of columns of B and C.
     * @param       CountK              Number of columns of A and rows of B.
     * @param       BlockCountK         Number of blocks between adjacent columns of the quantized B matrix.
     * @param       Bias                Bias vector of length N.
     * @param       ldc                 Number of elements between adjacent rows of C..
     * @param       ABlockSum           Supplies the blksum of A.
     * @param       QuantBBlkSum        Supplies the blksum of B.
     */
    typedef size_t(SQ8BitGemmKernel_BlkSum_CompInt8_Fn)(
        size_t BlkLen,
        const std::byte* QuantA,
        const float* QuantAScale,
        const std::byte* QuantBData,
        const float* QuantBScale,
        const std::byte* QuantBZeroPoint,
        float* C,
        size_t CountM,
        size_t CountN,
        size_t CountK,
        size_t BlockCountK,
        const float* Bias,
        size_t ldc,
        const float* ABlockSum,
        const float* QuantBBlkSum
    );

    SQ8BitGemmKernel_BlkSum_CompInt8_Fn* SQ8BitGemmKernel_BlkSum_CompInt8 = nullptr;

    /**
     * @brief Multiply quantized 8-bit integer matrix A with quantized 4-bit integer matrix B.
     *        A and B are block quantized and B is column major.
     *
     * @param       BlkLen              Number of values in a block.
     * @param       QuantA              Supplies the quantized A matrix.
                                        Binary data containing block quantized int8 data and scale values.
     * @param       QuantBData          Supplies the quantized B matrix block data.
     * @param       QuantBScale         Supplies the quantized B matrix block scale values.
     * @param       QuantBZeroPoint     Supplies the quantized B matrix block zero point values. Optional.
     * @param[out]  C                   Supplies the output C matrix.
     * @param       CountM              Number of rows of A and C to process, an upper bound.
     * @param       CountN              Number of columns of B and C to process.
     * @param       CountK              Number of columns of A and rows of B.
     * @param       BlockCountK         Number of blocks in one row of A and one column of B.
     * @param       ldc                 Number of elements between adjacent rows of C.
     * @param       Bias                Bias vector of length N.
     *
     * @return                          The number of rows of A and C that were processed, at most CountM.
     */
    typedef size_t(SQ4BitGemmKernel_CompInt8_Fn)(
        size_t BlkLen,
        const std::byte* QuantA,
        const std::byte* QuantBData,
        const float* QuantBScale,
        const std::byte* QuantBZeroPoint,
        float* C,
        size_t CountM,
        size_t CountN,
        size_t CountK,
        size_t BlockCountK,
        size_t ldc,
        const float* Bias
    );

    SQ4BitGemmKernel_CompInt8_Fn* SQ4BitGemmKernel_CompInt8 = nullptr;

    /**
     * @brief Whether to use SQ4BitGemmKernel_Packed_CompInt8 for this problem.
     */
    typedef bool(UsePacked_CompInt8_Fn)(
        size_t K,
        size_t BlkLen,
        bool HasZp
    );

    UsePacked_CompInt8_Fn* UsePacked_CompInt8 = nullptr;

    /**
     * @brief Block quantize values from matrix A from floats to quantized 8-bit integers.
     *        Used in conjunction with SQ4BitGemmKernel_Packed_CompInt8.
     *
     * @param       BlkLen  Number of values in a block.
     * @param       A       Supplies the A matrix.
     * @param       CountM  Number of rows of A.
     * @param       CountK  Number of columns of A.
     * @param[out]  QuantA  Supplies the output quantized A matrix.
     *                      Binary data containing block quantized int8 data and scale values.
     */
    typedef void(QuantizeA_Packed_CompInt8_Fn)(
        size_t BlkLen,
        const float* A,
        size_t CountM,
        size_t CountK,
        std::byte* QuantA
    );

    QuantizeA_Packed_CompInt8_Fn* QuantizeA_Packed_CompInt8 = nullptr;

    /**
     * @brief Block quantize values from one row of matrix A from floats to quantized 8-bit integers.
     *
     * @param       BlkLen  Number of values in a block.
     * @param       A       Supplies the A matrix.
     * @param       CountK  Number of columns of A.
     * @param[out]  QuantA  Supplies the output quantized A matrix.
     *                      Binary data containing block quantized int8 data and scale values.
     */
    typedef void(QuantizeARow_CompInt8_Fn)(
        size_t BlkLen,
        const float* A,
        size_t CountK,
        std::byte* QuantA
    );

    QuantizeARow_CompInt8_Fn* QuantizeARow_CompInt8 = nullptr;

    typedef void(QuantizeARowComputeBlkSum_CompInt8_Fn)(
        size_t BlkLen,
        const float* A,
        size_t CountK,
        std::byte* QuantA,
        float* QuantAScale,
        float* AScaledGroupSum  // scale_k * Sum_blklen(a_i)
    );
    QuantizeARowComputeBlkSum_CompInt8_Fn* QuantizeARowComputeBlkSum_CompInt8 = nullptr;

    /**
     * @brief Multiply fp16 matrix A rows with fp16 matrix B columns.
     *        Results are written to fp16 matrix C.
     *        If bias is provided, the bias are added to the result.
     *
     * @param       A                   first row of the A matrix segment. Row major.
     * @param       B                   first column of the B matrix segment. Column major.
     * @param       Bias                the bias at the target column. Optional.
     * @param[out]  C                   first element of the output matrix segment. Row major.
     * @param       CountM              the number of rows of A chunk.
     * @param       CountN              the number of columns of B chunk.
     * @param       K                   the number of columns of A matrix and rows of B matrix.
     * @param       lda                 the leading dimension of A.
     * @param       ldb                 the leading dimension of B.
     * @param       ldc                 the leading dimension of C.
     */
    typedef void(HQ4BitGemmKernel_CompFp16_Fn)(
        const MLAS_FP16* A,
        const MLAS_FP16* B,
        const MLAS_FP16* Bias,
        MLAS_FP16* C,
        size_t CountM,
        size_t CountN,
        size_t K,
        size_t lda,
        size_t ldb,
        size_t ldc
    );

    HQ4BitGemmKernel_CompFp16_Fn* HQ4BitGemmKernel_CompFp16 = nullptr;
};
