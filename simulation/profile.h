#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace ow::statescript::simulation {

// Globals 0x1854DF9 / 0x1854DF5. Distribution recording also enables totals.
struct ProfileRecordingFlags {
    bool distributions = false;
    bool aggregates = false;
    bool enabled() const { return distributions || aggregates; }
};

class SampleLevel {
public:
    SampleLevel() = default;
    SampleLevel(const SampleLevel&) = delete;
    SampleLevel& operator=(const SampleLevel&) = delete;
    SampleLevel(SampleLevel&&) = delete;
    SampleLevel& operator=(SampleLevel&&) = delete;
    static constexpr std::size_t capacity = 19;
    std::size_t size() const { return size_; }
    // Only indices below size() are initialized.
    float value(std::size_t index) const { return values_[index]; }

private:
    friend class HierarchicalSampleSketch;
    std::array<float, capacity> values_;
    std::uint8_t size_ = 0;
};

// 0x1031BF0 / 0x1031820. Storage is owned inline and never copied. Each
// sorted level retains at most19 samples; full levels promote their median.
// No quantile/percentile query is inferred from the recording functions.
class HierarchicalSampleSketch {
public:
    static constexpr std::size_t level_count = 4;
    static constexpr std::uint64_t sample_limit = 130321; // 19^4

    HierarchicalSampleSketch() = default;
    HierarchicalSampleSketch(const HierarchicalSampleSketch&) = delete;
    HierarchicalSampleSketch& operator=(const HierarchicalSampleSketch&) = delete;
    HierarchicalSampleSketch(HierarchicalSampleSketch&&) = delete;
    HierarchicalSampleSketch& operator=(HierarchicalSampleSketch&&) = delete;

    // False at the recovered sample limit; leaves all levels unchanged.
    bool add(float sample);
    std::uint64_t sample_count() const { return sample_count_; }
    const SampleLevel& level(std::size_t index) const { return levels_[index]; }

private:
    void insert_at_level(float sample, std::size_t level);
    std::array<SampleLevel, level_count> levels_;
    std::uint64_t sample_count_ = 0;
};

struct AggregateProfileRecord {
    std::uint64_t key;
    std::uint32_t subject_id; // exact meaning unresolved; not assumed an ability id
    std::uint16_t tag;
    float accumulated_sample;
    float pending_sample;
    std::uint32_t sample_count;
};

struct DistributionProfileRecord {
    explicit DistributionProfileRecord(std::uint64_t value) : key(value) {}
    std::uint64_t key;
    HierarchicalSampleSketch samples;
};

// Independent storage, not the beta's binary layout. Records are allocated
// only when new; distribution records have stable owned storage so growing a
// bucket never copies sketches or reads unused float slots. Existing-record
// samples allocate nothing. Access/record/commit are serialized by the caller.
class SimulationProfile {
public:
    static constexpr std::size_t bucket_count = 64;
    SimulationProfile() = default;
    SimulationProfile(const SimulationProfile&) = delete;
    SimulationProfile& operator=(const SimulationProfile&) = delete;
    SimulationProfile(SimulationProfile&&) = default;
    SimulationProfile& operator=(SimulationProfile&&) = default;

    // 0xC303A0 -> 0x1031630/0x1031730. Key0 NEVER matches an old record.
    void record(ProfileRecordingFlags, std::uint64_t key, float sample,
                std::uint16_t tag, std::uint32_t subject_id);
    // 0x1031500: folds pending into accumulated and advances the frame count
    // only when either recording flag is enabled. Does not clear record keys,
    // sample counts or distribution levels when recording is disabled.
    void commit_frame(ProfileRecordingFlags);

    std::uint64_t committed_frames() const { return committed_frames_; }
    const std::vector<AggregateProfileRecord>& aggregate_bucket(std::uint64_t key) const {
        return aggregates_[key & 63u];
    }
    std::size_t distribution_bucket_size(std::uint64_t key) const {
        return distributions_[key & 63u].size();
    }
    const DistributionProfileRecord& distribution_record(std::uint64_t key,
                                                         std::size_t index) const {
        return *distributions_[key & 63u][index];
    }

private:
    void add_distribution_sample(std::uint64_t key, float sample);
    void accumulate_tagged_sample(std::uint64_t key, float sample,
                                  std::uint16_t tag, std::uint32_t subject_id);
    std::array<std::vector<AggregateProfileRecord>, bucket_count> aggregates_;
    std::array<std::vector<std::unique_ptr<DistributionProfileRecord>>, bucket_count>
        distributions_;
    std::uint64_t committed_frames_ = 0;
};

} // namespace ow::statescript::simulation
