#include "cuda_runtime.hpp"
#include <windows.h>
#include <iostream>

int main(int argc, char **) {
    try {
        std::cout << cuda_driver_status() << std::endl;
        if (GetModuleHandleW(L"cublas64_12.dll") || GetModuleHandleW(L"cublasLt64_12.dll") ||
            GetModuleHandleW(L"cudart64_12.dll"))
            throw std::runtime_error("CUDA runtime loaded before first GPU request");
        if (argc == 1)
            return 0;
        ensure_cuda_runtime({}, [](std::string status) { std::cout << status << std::endl; });
        if (!GetModuleHandleW(L"cublas64_12.dll") || !GetModuleHandleW(L"cublasLt64_12.dll"))
            throw std::runtime_error("CUDA libraries were not loaded");
        std::cout << "PASS: CUDA runtime initialized on demand\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
}
