/*
 * batcher.actor.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

// When actually compiled (NO_INTELLISENSE), include the generated version of this file.  In intellisense use the source version.
#if defined(NO_INTELLISENSE) && !defined(FLOW_BATCHER_ACTOR_G_H)
	#define FLOW_BATCHER_ACTOR_G_H
	#include "fdbrpc/batcher.actor.g.h"
#elif !defined(FLOW_BATCHER_ACTOR_H)
	#define FLOW_BATCHER_ACTOR_H

#include "fdbrpc/Stats.h"
#include "flow/flow.h"
#include "flow/actorcompiler.h"  // This must be the last #include.

template <class X>
void logOnReceive(X x) { }

void logOnReceive(CommitTransactionRequest x) {
	if(x.debugID.present())
		g_traceBatch.addEvent("CommitDebug", x.debugID.get().first(), "MasterProxyServer.batcher");
}

template <class X>
bool firstInBatch(X x) { return false; }

bool firstInBatch(CommitTransactionRequest x) {
	return x.firstInBatch();
}

ACTOR template <class X>
Future<Void> batcher(PromiseStream<std::pair<std::vector<X>, int> > out, FutureStream<X> in, double avgMinDelay, double* avgMaxDelay, double emptyBatchTimeout, int maxCount, int desiredBytes, int maxBytes, Optional<PromiseStream<Void>> batchStartedStream, int64_t *commitBatchesMemBytesCount, int64_t commitBatchesMemBytesLimit, TaskPriority taskID = TaskPriority::DefaultDelay, Counter* counter = 0)
{
	wait( delayJittered(*avgMaxDelay, taskID) );  // smooth out
	// This is set up to deliver even zero-size batches if emptyBatchTimeout elapses, because that's what master proxy wants.  The source control history
	// contains a version that does not.

	state double lastBatch = 0;

	loop {
		state Future<Void> timeout;
		state std::vector<X> batch;
		state int batchBytes = 0;

		if(emptyBatchTimeout <= 0)
			timeout = Never();
		else
			timeout = delayJittered(emptyBatchTimeout, taskID);

		while (!timeout.isReady() && !(batch.size() == maxCount || batchBytes >= desiredBytes)) {
			choose {
				when ( X x  = waitNext(in) ) {
					int bytes = getBytes(x);
					// Drop requests if memory is under severe pressure
					if (*commitBatchesMemBytesCount + bytes > commitBatchesMemBytesLimit) {
						x.reply.sendError(proxy_memory_limit_exceeded());
						TraceEvent(SevWarnAlways, "ProxyCommitBatchMemoryThresholdExceeded").suppressFor(60).detail("CommitBatchesMemBytesCount", *commitBatchesMemBytesCount).detail("CommitBatchesMemLimit", commitBatchesMemBytesLimit);
						continue;
					}

					// Process requests in the normal case
					if (counter) ++*counter;
					logOnReceive(x);
					if (!batch.size()) {
						if(batchStartedStream.present())
							batchStartedStream.get().send(Void());
						if (now() - lastBatch > *avgMaxDelay)
							timeout = delayJittered(avgMinDelay, taskID);
						else
							timeout = delayJittered(*avgMaxDelay - (now() - lastBatch), taskID);
					}

					bool first = firstInBatch( x );
					if((batchBytes + bytes > maxBytes || first) && batch.size()) {
						out.send({ batch, batchBytes });
						lastBatch = now();
						if(batchStartedStream.present())
							batchStartedStream.get().send(Void());
						timeout = delayJittered(*avgMaxDelay, taskID);
						batch = std::vector<X>();
						batchBytes = 0;
					}

					batch.push_back(x);
					batchBytes += bytes;
					*commitBatchesMemBytesCount += bytes;
				}
				when ( wait( timeout ) ) {}
			}
		}
		out.send({std::move(batch), batchBytes});
		lastBatch = now();
	}
}

#include "flow/unactorcompiler.h"

#endif
