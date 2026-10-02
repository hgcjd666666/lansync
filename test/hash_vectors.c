/*
 * hash_vectors.c -- 校验 third_party 里那个 gxhash 与上游行为一致
 *
 * 为什么需要它：整个内容哈希方案建立在一个前提上——**同一段字节在服务端和
 * 客户端算出同一个值**。gxhash 的 README 说「同一大版本内跨平台稳定」，
 * 但那是别人家的承诺，我们把它转移成了自己二进制的正确性依赖。
 * 这里直接用上游 is_stable 测试的原始期望值钉死它，换 .a 或改编译参数
 * 都会立刻炸出来。
 *
 * 这些期望值抄自 gxhash 仓库 src/gxhash/mod.rs 的 is_stable 测试：
 *   data[i] = (i * 31 + 7) as u8，种子 [0, 42, -1, i64::MIN]。
 *
 * 用法：make test-hash
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* third_party/libgxhash_arm64.a 导出的 C 接口 */
extern uint32_t gxhash32(const void *buf, size_t len, int64_t seed);
extern uint64_t gxhash64(const void *buf, size_t len, int64_t seed);

static const struct {
    size_t   len;
    uint64_t h[4];
} EXP[] = {
    {0,    {0x9e2a74d6323c1e7eULL, 0xd638e4bb0b02c811ULL, 0xed134192d1162902ULL, 0x5e3515741951d182ULL}},
    {1,    {0xbd1d4a1906c5d74aULL, 0xfd039c40ccc371b6ULL, 0x7ee15d54f5363997ULL, 0xf4945e7dab41f445ULL}},
    {7,    {0x7b52fc7f0b725c24ULL, 0x3af624c643b91323ULL, 0x3d5bde93db94bed4ULL, 0x935d237644019699ULL}},
    {15,   {0x102cac080b0aaa2fULL, 0x62054297e134d44fULL, 0x8e68b70dda47714dULL, 0x9ca5ba6ced8c590aULL}},
    {16,   {0xe15d40b9c011ade5ULL, 0xffc7d0191ea25a21ULL, 0x27016e699ed0947aULL, 0x426f7b5b3cc5e8feULL}},
    {17,   {0x40791975698e21b9ULL, 0x088af766ea213939ULL, 0x7b8cc3a63d500b3bULL, 0xe66543dcdbc639baULL}},
    {31,   {0xf6b8d10583aff73dULL, 0x002835620b7a9da3ULL, 0xd4d13870ab672dcdULL, 0x672940b6f9a96639ULL}},
    {32,   {0xb5537031f3aac5e6ULL, 0xad5b853ba5ec1b91ULL, 0xa0e900022dd1dd61ULL, 0x0892398b93f46feeULL}},
    {33,   {0x9b0f0e8ff1f2a1e3ULL, 0x8081e03938e07534ULL, 0x2f3a86e4ef700ec4ULL, 0xefeef9375d900a3bULL}},
    {48,   {0xca85ec03c3b4c895ULL, 0xcf187c3b94aa71d2ULL, 0xce07b046f69bce61ULL, 0x868720b88891d1f0ULL}},
    {49,   {0xaf2aafa09a85e34dULL, 0xa8d482de6c246dacULL, 0xdd4b388229918521ULL, 0x4ae234a2a904edcdULL}},
    {64,   {0x4ead24efb9175e7fULL, 0x7df7bd82c441b97aULL, 0xca293a74699ce2adULL, 0x9cf06b773e171912ULL}},
    {65,   {0xc15aa42eb759647eULL, 0x810e86cf1c97cde2ULL, 0x115651a649cad9faULL, 0x61e911fed6c80240ULL}},
    {96,   {0x5445b21eb455d334ULL, 0xb992281d6a3a524aULL, 0xa63a91ca55a2daa9ULL, 0x607d9cf50ce6eb71ULL}},
    {97,   {0x9a158f9c19c009d8ULL, 0x4bea47008ddff769ULL, 0xffe65eb56b966166ULL, 0x4fc8ab14afe1cf9aULL}},
    {128,  {0x1171957e32035f02ULL, 0x77b7ecce0299a2bbULL, 0xcde87a297a20af8cULL, 0x946067321102656fULL}},
    {129,  {0xbe4a24e6191efca2ULL, 0xb16365ebf4c609f5ULL, 0x6eb45e35d5dd0e81ULL, 0xcb666635bf4d821eULL}},
    {256,  {0x34b670299cd81782ULL, 0x00370d479a205b09ULL, 0xc59ac15f702127d9ULL, 0x01dfbe8d63e79eabULL}},
    {257,  {0x30f5d0e8c1a96b51ULL, 0xd1f653fa179697dfULL, 0x451164ed40e36944ULL, 0x8448d8ca4069fff7ULL}},
    {1024, {0x104643c594a0e8afULL, 0x3211829ad48bbeb0ULL, 0x2be12306d9469dabULL, 0xa128dea3318d0954ULL}},
    {1025, {0xa6f201c56698e466ULL, 0x7cd067502cd74948ULL, 0x25d0e13ba0f559d5ULL, 0x99084eceaf21451cULL}},
    {4242, {0x5417103f34a4621aULL, 0xf87cf5c9f35c92b2ULL, 0xc4d3ceee561116d6ULL, 0xecb1a829277b7663ULL}},
};

static const int64_t SEEDS[4] = {0, 42, -1, INT64_MIN};

int main(void)
{
    static unsigned char data[4242];
    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = (unsigned char)(i * 31 + 7);
    }

    int fails = 0;
    size_t n = sizeof(EXP) / sizeof(EXP[0]);

    for (size_t k = 0; k < n; k++) {
        for (int s = 0; s < 4; s++) {
            uint64_t got = gxhash64(data, EXP[k].len, SEEDS[s]);
            if (got != EXP[k].h[s]) {
                printf("FAIL len=%zu seed=%lld got=%016llx want=%016llx\n", EXP[k].len,
                       (long long)SEEDS[s], (unsigned long long)got,
                       (unsigned long long)EXP[k].h[s]);
                fails++;
            }
        }
    }

    /* gxhash32 与 128 位版本也各钉一个值 */
    uint32_t h32 = gxhash32("Hello World", 11, 0);
    if (h32 != 3930652002u) {
        printf("FAIL gxhash32(\"Hello World\", 0) got=%u want=3930652002\n", h32);
        fails++;
    }

    if (fails > 0) {
        printf("\n哈希向量不匹配：%d 处。\n", fails);
        printf("这说明 third_party 里的 gxhash 和上游行为不一致，\n");
        printf("两端可能算出不同的哈希——不要提交，先查 .a 是怎么来的。\n");
        return 1;
    }

    printf("gxhash 向量全部匹配：%zu 个长度 x 4 个种子 + gxhash32\n", n);
    return 0;
}
