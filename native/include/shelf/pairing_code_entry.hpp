// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "managed_tile_pairing_client.hpp"
#include <cstddef>
#include <cstdint>
#include <limits>

namespace shelf {

// Controller-only entry model for a future opt-in pairing view. No Settings,
// SDL text input, native IME, filesystem, network, or logging dependency.
class PairingCodeEntry {
  public:
    static constexpr std::size_t code_length = managed_tile_pairing_client::code_bytes;
    static constexpr std::uint64_t max_lifetime_ms = 120000;
    static constexpr char alphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    enum class Command { up, down, left, right, accept, erase, clear, cancel };
    enum class Result { inactive, unchanged, updated, complete, invalid, expired, cancelled };

    PairingCodeEntry() = default;
    PairingCodeEntry(const PairingCodeEntry&) = delete;
    PairingCodeEntry& operator=(const PairingCodeEntry&) = delete;
    ~PairingCodeEntry() { cancel(); }

    bool begin(std::uint64_t now_ms, std::uint64_t lifetime_ms) {
        cancel();
        if (!lifetime_ms || lifetime_ms > max_lifetime_ms ||
            now_ms > std::numeric_limits<std::uint64_t>::max() - lifetime_ms)
            return false;
        last_ms_ = now_ms;
        expires_ms_ = now_ms + lifetime_ms;
        active_ = true;
        return true;
    }

    // A regressed or expired monotonic clock clears entered symbols immediately.
    bool active(std::uint64_t now_ms) {
        if (!active_)
            return false;
        if (now_ms < last_ms_ || now_ms >= expires_ms_) {
            cancel();
            return false;
        }
        last_ms_ = now_ms;
        return true;
    }

    std::size_t size(std::uint64_t now_ms) { return active(now_ms) ? size_ : 0; }
    unsigned selection(std::uint64_t now_ms) { return active(now_ms) ? selected_ : 0; }
    char symbol(std::uint64_t now_ms, std::size_t position) {
        return active(now_ms) && position < size_ ? symbols_[position] : '\0';
    }

    Result apply(Command command, std::uint64_t now_ms) {
        if (!active_)
            return Result::inactive;
        if (!active(now_ms))
            return Result::expired;
        if (command == Command::cancel) {
            cancel();
            return Result::cancelled;
        }
        if (command == Command::clear) {
            zero_symbols();
            return Result::updated;
        }
        if (command == Command::erase) {
            if (!size_)
                return Result::unchanged;
            managed_tile_pairing_client::clear(&symbols_[--size_], 1);
            return Result::updated;
        }
        if (command == Command::accept) {
            if (size_ == code_length)
                return Result::complete;
            const char choice = alphabet[selected_];
            // A 96-bit secret uses 100 code bits; the last four are zero.
            if (size_ == code_length - 1 && choice != '0' && choice != 'G')
                return Result::invalid;
            symbols_[size_++] = choice;
            return size_ == code_length ? Result::complete : Result::updated;
        }
        const unsigned previous = selected_;
        // Eight columns by four rows; movement is bounded, without wrapping.
        if (command == Command::left && selected_ % 8)
            --selected_;
        else if (command == Command::right && selected_ % 8 != 7)
            ++selected_;
        else if (command == Command::up && selected_ >= 8)
            selected_ -= 8;
        else if (command == Command::down && selected_ < 24)
            selected_ += 8;
        return selected_ == previous ? Result::unchanged : Result::updated;
    }

    // The only transfer operation. Caller owns and must clear the decoded
    // secret when finished. The entry buffer is cleared even on failure.
    bool take_secret(std::uint64_t now_ms, managed_tile_pairing_client::Secret& output) {
        managed_tile_pairing_client::clear(output);
        if (!active(now_ms))
            return false;
        const bool valid = size_ == code_length &&
            managed_tile_pairing_client::decode_code(symbols_, size_, output);
        cancel();
        return valid;
    }

    void cancel() {
        zero_symbols();
        selected_ = 0;
        last_ms_ = 0;
        expires_ms_ = 0;
        active_ = false;
    }

  private:
    void zero_symbols() {
        managed_tile_pairing_client::clear(symbols_, sizeof(symbols_));
        size_ = 0;
    }
    char symbols_[code_length + 1]{};
    std::size_t size_ = 0;
    unsigned selected_ = 0;
    std::uint64_t last_ms_ = 0, expires_ms_ = 0;
    bool active_ = false;
};

} // namespace shelf
