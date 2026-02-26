#ifndef SHA256_H
#define SHA256_H

#include <string>
#include <iomanip>
#include <sstream>
#include <openssl/evp.h> // <--- 헤더 변경 (sha.h -> evp.h)

// 문자열을 입력받아 SHA-256 해시 문자열(64자리)을 반환
inline std::string sha256(const std::string str)
{
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int lengthOfHash = 0;

    // 1. Context 생성
    EVP_MD_CTX *context = EVP_MD_CTX_new();

    if (context != nullptr)
    {
        // 2. 초기화 (SHA256 알고리즘 사용 설정)
        if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr))
        {
            // 3. 데이터 업데이트
            if (EVP_DigestUpdate(context, str.c_str(), str.size()))
            {
                // 4. 해시 생성 완료
                EVP_DigestFinal_ex(context, hash, &lengthOfHash);
            }
        }
        // 5. Context 해제 (메모리 누수 방지)
        EVP_MD_CTX_free(context);
    }

    // 16진수 문자열로 변환 (기존과 동일)
    std::stringstream ss;
    for (unsigned int i = 0; i < lengthOfHash; i++)
    {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    }
    return ss.str();
}

#endif