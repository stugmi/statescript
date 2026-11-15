#include "profile.h"
#include <utility>

namespace ow::statescript::simulation {

bool HierarchicalSampleSketch::add(float sample) {
    if (sample_count_ >= sample_limit)
        return false;
    insert_at_level(sample, 0);
    ++sample_count_;
    return true;
}

void HierarchicalSampleSketch::insert_at_level(float sample, std::size_t index) {
    SampleLevel& level = levels_[index];
    if (level.size_ == SampleLevel::capacity) {
        // 0x103186C promotes the existing sorted median, NOT the new sample.
        // The 19^4 input limit prevents promotion beyond the four levels.
        insert_at_level(level.values_[level.size_ / 2], index + 1);
        level.size_ = 0;
    }

    // Match the original count-1 bounded search, including its float
    // comparisons. std::lower_bound's '<' predicate differs for NaNs.
    const std::size_t count = level.size_;
    std::size_t position = 0;
    if (count != 0) {
        const std::size_t last = count - 1;
        std::size_t high = last;
        while (position < high) {
            const std::size_t middle = (position + high) / 2;
            if (sample <= level.values_[middle])
                high = middle;
            else
                position = middle + 1;
        }
        if (position == last && !(level.values_[position] >= sample))
            position = count;
    }
    for (std::size_t i = count; i > position; --i)
        level.values_[i] = level.values_[i - 1];
    level.values_[position] = sample;
    ++level.size_;
}

void SimulationProfile::record(ProfileRecordingFlags flags, std::uint64_t key,
                               float sample, std::uint16_t tag,
                               std::uint32_t subject_id) {
    if (flags.distributions)
        add_distribution_sample(key, sample);
    if (flags.enabled())
        accumulate_tagged_sample(key, sample, tag, subject_id);
}

void SimulationProfile::add_distribution_sample(std::uint64_t key, float sample) {
    auto& bucket = distributions_[key & 63u];
    if (key != 0) {
        for (const auto& record : bucket) {
            if (record->key == key) {
                record->samples.add(sample);
                return;
            }
        }
    }
    auto record = std::make_unique<DistributionProfileRecord>(key);
    record->samples.add(sample);
    bucket.push_back(std::move(record));
}

void SimulationProfile::accumulate_tagged_sample(std::uint64_t key, float sample,
                                                std::uint16_t tag,
                                                std::uint32_t subject_id) {
    auto& bucket = aggregates_[key & 63u];
    if (key != 0) {
        for (auto& record : bucket) {
            if (record.key == key && record.subject_id == subject_id &&
                record.tag == tag) {
                record.pending_sample += sample;
                ++record.sample_count;
                return;
            }
        }
    }
    // 0x10317E5 puts the FIRST sample directly into accumulated. Only later
    // samples go into pending; committing the frame must not double-count it.
    bucket.push_back({key, subject_id, tag, sample, 0.0f, 1});
}

void SimulationProfile::commit_frame(ProfileRecordingFlags flags) {
    if (!flags.enabled())
        return;
    for (auto& bucket : aggregates_) {
        for (auto& record : bucket) {
            record.accumulated_sample += record.pending_sample;
            record.pending_sample = 0.0f;
        }
    }
    ++committed_frames_;
}

} // namespace ow::statescript::simulation
