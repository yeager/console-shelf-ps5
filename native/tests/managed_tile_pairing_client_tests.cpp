#include "shelf/managed_tile_pairing_client.hpp"

#include <cassert>
#include <cstring>
#include <string>

namespace pair = shelf::managed_tile_pairing_client;

static std::string hex(const pair::Mac& bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (unsigned char byte : bytes) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
}

static void hmac_vectors() {
    pair::Mac mac{};
    unsigned char key[20];
    std::memset(key, 0x0b, sizeof(key));
    constexpr char message[] = "Hi There";
    const pair::Part first[]{{message, sizeof(message) - 1}};
    assert(pair::hmac(key, sizeof(key), first, 1, mac));
    assert(hex(mac) == "b0344c61d8db38535ca8afceaf0bf12b"
                       "881dc200c9833da726e9376c2e32cff7");

    unsigned char long_key[131];
    std::memset(long_key, 0xaa, sizeof(long_key));
    constexpr char long_message[] =
        "Test Using Larger Than Block-Size Key - Hash Key First";
    const pair::Part second[]{{long_message, sizeof(long_message) - 1}};
    assert(pair::hmac(long_key, sizeof(long_key), second, 1, mac));
    assert(hex(mac) == "60e431591ee0b67f0d8a26aacbf5b77f"
                       "8e0bc6213728c5140546040f0ee37f54");
    assert(!pair::hmac(nullptr, sizeof(key), first, 1, mac));
    assert(hex(mac) == std::string(64, '0'));
}

static void transcript_vectors() {
    pair::Secret secret{};
    pair::Nonce server_nonce{};
    pair::Nonce client_nonce{};
    for (std::size_t i = 0; i < secret.size(); ++i)
        secret[i] = static_cast<unsigned char>(i + 1);
    for (std::size_t i = 0; i < server_nonce.size(); ++i) {
        server_nonce[i] = static_cast<unsigned char>(0x40 + i);
        client_nonce[i] = static_cast<unsigned char>(0xa0 + i);
    }
    pair::Secret decoded{};
    constexpr char code[] = "041061050R3GG28A1C60";
    assert(pair::decode_code(code, sizeof(code) - 1, decoded));
    assert(decoded == secret);
    decoded.fill(0xa5);
    assert(!pair::decode_code(code, sizeof(code) - 2, decoded));
    assert(decoded == pair::Secret{});
    char changed_code[sizeof(code)];
    std::memcpy(changed_code, code, sizeof(code));
    changed_code[0] = 'O';
    assert(!pair::decode_code(changed_code, sizeof(code) - 1, decoded));
    changed_code[0] = '0';
    changed_code[sizeof(code) - 2] = '1'; // Nonzero trailing padding.
    assert(!pair::decode_code(changed_code, sizeof(code) - 1, decoded));
    std::memcpy(changed_code, code, sizeof(code));
    changed_code[5] = 'a'; // Only canonical uppercase input is accepted.
    assert(!pair::decode_code(changed_code, sizeof(code) - 1, decoded));
    changed_code[5] = '\0';
    assert(!pair::decode_code(changed_code, sizeof(code) - 1, decoded));
    assert(decoded == pair::Secret{});

    pair::Mac client_proof{};
    assert(pair::client_proof(secret, server_nonce, client_nonce, client_proof));
    assert(hex(client_proof) == "bcaa31c958591012e4c01e3b951acb50"
                                "b71b85784f2a043cff3845dd36a6a31d");
    pair::Mac server_proof{};
    constexpr char proof_domain[] = "ISHELF-PAIR-SERVER-V1";
    assert(pair::pair_mac(secret, proof_domain, sizeof(proof_domain) - 1,
                          server_nonce, client_nonce, server_proof));
    assert(hex(server_proof) == "17ccf46c3f3796c438c291da73444ade"
                                "aabdce9c3caed5ddd505ce5c0e0e2c1b");
    pair::Session session{};
    pair::Mac changed_proof = server_proof;
    changed_proof[4] ^= 1;
    session.active = true;
    session.next_counter = 99;
    session.key.fill(0xa5);
    assert(!pair::finish(secret, server_nonce, client_nonce, changed_proof, session));
    assert(!session.active && session.next_counter == 0);
    assert(hex(session.key) == std::string(64, '0'));
    assert(pair::finish(secret, server_nonce, client_nonce, server_proof, session));
    assert(session.active && session.next_counter == 1);
    assert(hex(session.key) == "e0df44caed690306b87cf2a8c361b52f"
                               "142fedb8c487aa044a2805fb87533ed0");

    constexpr char path[] = "/api/v1/isshelf/tiles/create";
    constexpr char body[] = "{\"contract_version\":1}";
    pair::Mac request{};
    std::uint64_t counter = 99;
    assert(!pair::prepare_request(session, "/api/v1/manual/add", 18,
                                  body, sizeof(body) - 1, counter, request));
    assert(counter == 0 && session.next_counter == 1);
    assert(hex(request) == std::string(64, '0'));
    assert(pair::prepare_request(session, path, sizeof(path) - 1,
                                 body, sizeof(body) - 1, counter, request));
    assert(counter == 1 && session.next_counter == 2);
    assert(hex(request) == "e2ee0823bff8031fd27f7d0f56f7277c"
                           "2edb16b4e0915114e53b72c86e44c1c6");
    constexpr char reply[] = "{\"status\":\"COMMITTED\",\"operation_id\":\"abc\"}";
    pair::Mac response{};
    assert(pair::response_mac(session, counter, request, 200,
                              reply, sizeof(reply) - 1, response));
    assert(hex(response) == "d251fd238f70883aa2fe4e3409c3151c"
                            "d9c998206d0b95f559d9aa175f8df9b2");
    assert(pair::verify_response(session, counter, request, 200,
                                 reply, sizeof(reply) - 1, response));
    assert(!pair::verify_response(session, counter, request, 500,
                                  reply, sizeof(reply) - 1, response));
    assert(!pair::verify_response(session, counter + 1, request, 200,
                                  reply, sizeof(reply) - 1, response));
    pair::Mac changed_request = request;
    changed_request[0] ^= 1;
    assert(!pair::verify_response(session, counter, changed_request, 200,
                                  reply, sizeof(reply) - 1, response));
    assert(!pair::verify_response(session, counter, request, 200,
                                  reply, sizeof(reply) - 2, response));
    assert(!pair::verify_response(session, counter, request, 200,
                                  reply, sizeof(reply) - 1, pair::Mac{}));
    assert(!pair::response_mac(session, counter, request, 99,
                               reply, sizeof(reply) - 1, response));
    assert(hex(response) == std::string(64, '0'));
    // Treat a missing transport response as unknown: the next request burns 2.
    constexpr char status_path[] = "/api/v1/isshelf/tiles/status";
    constexpr char status_body[] = "{}";
    assert(pair::prepare_request(session, status_path, sizeof(status_path) - 1,
                                 status_body, sizeof(status_body) - 1,
                                 counter, request));
    assert(counter == 2 && session.next_counter == 3);
    assert(hex(request) != "e2ee0823bff8031fd27f7d0f56f7277c"
                           "2edb16b4e0915114e53b72c86e44c1c6");
    session.next_counter = UINT64_MAX;
    assert(!pair::prepare_request(session, status_path, sizeof(status_path) - 1,
                                  status_body, sizeof(status_body) - 1,
                                  counter, request));
    assert(counter == 0 && session.next_counter == UINT64_MAX);
    assert(hex(request) == std::string(64, '0'));
    pair::clear(secret);
    pair::clear(session);
    assert(!session.active && session.next_counter == 0);
    assert(hex(session.key) == std::string(64, '0'));
}

int main() {
    hmac_vectors();
    transcript_vectors();
}
