#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace GakuMotion {

struct CompletedTrackCover {
	std::string key;
	bool hasCover{false};
	std::string coverDataUri;
};

// Single-worker background cache for track cover data URIs.
// Supports positive and negative caching so repeated queries for tracks without
// artwork never re-run disk or FFmpeg probing on the DesktopUI thread.
class TrackCoverCache {
public:
	using ResolverFn = std::function<std::string(const std::string& key)>;

	TrackCoverCache() = default;
	~TrackCoverCache() { Stop(); }

	TrackCoverCache(const TrackCoverCache&) = delete;
	TrackCoverCache& operator=(const TrackCoverCache&) = delete;

	void Configure(ResolverFn resolver) {
		Stop();
		std::lock_guard<std::mutex> lock(mutex_);
		stopping_ = false;
		resolver_ = std::move(resolver);
		entries_.clear();
		completed_.clear();
		queue_.clear();
		activeOrQueued_.clear();
		keyGeneration_.clear();
		++globalGeneration_;
	}

	void Stop() {
		std::thread workerToJoin;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			stopping_ = true;
			queue_.clear();
			activeOrQueued_.clear();
			if (worker_.joinable()) {
				workerToJoin = std::move(worker_);
			}
		}
		cv_.notify_all();
		if (workerToJoin.joinable()) {
			workerToJoin.join();
		}
	}

	bool QueryOrEnqueue(const std::string& cacheKey, const std::string& resolveArg,
						std::string& outDataUri, bool& outPending) {
		outDataUri.clear();
		outPending = false;
		if (cacheKey.empty()) {
			return false;
		}
		std::lock_guard<std::mutex> lock(mutex_);
		const auto found = entries_.find(cacheKey);
		if (found != entries_.end()) {
			outDataUri = found->second;
			outPending = false;
			return true;
		}
		if (stopping_ || !resolver_) {
			return false;
		}
		const uint64_t keyGen = keyGeneration_[cacheKey];
		const auto currentStamp = std::make_pair(globalGeneration_, keyGen);
		auto it = activeOrQueued_.find(cacheKey);
		if (it == activeOrQueued_.end() || it->second != currentStamp) {
			activeOrQueued_[cacheKey] = currentStamp;
			queue_.push_back(QueuedItem{cacheKey, resolveArg.empty() ? cacheKey : resolveArg, globalGeneration_, keyGen});
			EnsureWorkerLocked();
			cv_.notify_one();
		}
		outPending = true;
		return true;
	}

	bool QueryOrEnqueue(const std::string& key, std::string& outDataUri, bool& outPending) {
		return QueryOrEnqueue(key, key, outDataUri, outPending);
	}

	void Invalidate(const std::string& key) {
		if (key.empty()) return;
		std::lock_guard<std::mutex> lock(mutex_);
		entries_.erase(key);
		++keyGeneration_[key];
		completed_.erase(
			std::remove_if(completed_.begin(), completed_.end(),
				[&](const CompletedTrackCover& c) { return c.key == key; }),
			completed_.end());
	}

	void InvalidateAll() {
		std::lock_guard<std::mutex> lock(mutex_);
		entries_.clear();
		completed_.clear();
		queue_.clear();
		activeOrQueued_.clear();
		keyGeneration_.clear();
		++globalGeneration_;
	}

	std::vector<CompletedTrackCover> DrainCompleted(size_t maxCount = 4) {
		std::lock_guard<std::mutex> lock(mutex_);
		std::vector<CompletedTrackCover> out;
		if (maxCount == 0 || completed_.empty()) return out;
		const size_t count = (maxCount < completed_.size()) ? maxCount : completed_.size();
		out.reserve(count);
		for (size_t i = 0; i < count; ++i) {
			out.push_back(std::move(completed_.front()));
			completed_.pop_front();
		}
		return out;
	}

	void WaitForIdleForTest() {
		std::unique_lock<std::mutex> lock(mutex_);
		idleCv_.wait(lock, [&] { return queue_.empty() && activeCount_ == 0; });
	}

private:
	struct QueuedItem {
		std::string key;
		std::string resolveArg;
		uint64_t globalGen{0};
		uint64_t keyGen{0};
	};

	void EnsureWorkerLocked() {
		if (worker_.joinable() || stopping_) return;
		worker_ = std::thread([this]() { WorkerLoop(); });
	}

	void WorkerLoop() {
		for (;;) {
			QueuedItem item;
			ResolverFn resolverCopy;
			{
				std::unique_lock<std::mutex> lock(mutex_);
				cv_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
				if (stopping_) {
					idleCv_.notify_all();
					return;
				}
				item = std::move(queue_.front());
				queue_.pop_front();
				resolverCopy = resolver_;
				++activeCount_;
			}

			std::string uri;
			if (resolverCopy) {
				try {
					uri = resolverCopy(item.resolveArg);
				} catch (...) {
					uri.clear();
				}
			}

			{
				std::lock_guard<std::mutex> lock(mutex_);
				--activeCount_;
				const auto stamp = std::make_pair(item.globalGen, item.keyGen);
				const auto qIt = activeOrQueued_.find(item.key);
				if (qIt != activeOrQueued_.end() && qIt->second == stamp) {
					activeOrQueued_.erase(qIt);
				}
				const uint64_t currentKeyGen = keyGeneration_[item.key];
				if (!stopping_ &&
					item.globalGen == globalGeneration_ &&
					item.keyGen == currentKeyGen) {
					entries_[item.key] = uri;
					CompletedTrackCover done;
					done.key = item.key;
					done.hasCover = !uri.empty();
					done.coverDataUri = uri;
					completed_.push_back(std::move(done));
				}
				if (queue_.empty() && activeCount_ == 0) {
					idleCv_.notify_all();
				}
			}
		}
	}

	mutable std::mutex mutex_;
	std::condition_variable cv_;
	std::condition_variable idleCv_;
	std::thread worker_;
	bool stopping_{false};
	size_t activeCount_{0};
	uint64_t globalGeneration_{0};
	ResolverFn resolver_;
	std::unordered_map<std::string, std::string> entries_;
	std::unordered_map<std::string, uint64_t> keyGeneration_;
	std::unordered_map<std::string, std::pair<uint64_t, uint64_t>> activeOrQueued_;
	std::deque<QueuedItem> queue_;
	std::deque<CompletedTrackCover> completed_;
};

} // namespace GakuMotion
