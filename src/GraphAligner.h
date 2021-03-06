#ifndef GraphAligner_H
#define GraphAligner_H

#include <chrono>
#include <algorithm>
#include <string>
#include <vector>
#include <iostream>
#include "AlignmentGraph.h"
#include "CommonUtils.h"
#include "GraphAlignerWrapper.h"
#include "ThreadReadAssertion.h"
#include "GraphAlignerCommon.h"
#include "GraphAlignerVGAlignment.h"
#include "GraphAlignerBitvectorBanded.h"

template <typename LengthType, typename ScoreType, typename Word>
class GraphAligner
{
private:
	using VGAlignment = GraphAlignerVGAlignment<LengthType, ScoreType, Word>;
	using BitvectorAligner = GraphAlignerBitvectorBanded<LengthType, ScoreType, Word>;
	using Common = GraphAlignerCommon<LengthType, ScoreType, Word>;
	using Params = typename Common::Params;
	using MatrixPosition = typename Common::MatrixPosition;
	using Trace = typename Common::Trace;
	using OnewayTrace = typename Common::OnewayTrace;
	BitvectorAligner bvAligner;
	mutable BufferedWriter logger;
	using AlignerGraphsizedState = typename Common::AlignerGraphsizedState;
	using TraceItem = typename Common::TraceItem;
	const Params& params;
public:

	GraphAligner(const Params& params) :
	bvAligner(params),
	logger(),
	params(params)
	{
		if (!params.quietMode) logger = { std::cerr };
	}
	
	AlignmentResult AlignOneWay(const std::string& seq_id, const std::string& sequence, AlignerGraphsizedState& reusableState) const
	{
		AlignmentResult result;
		result.readName = seq_id;
		auto timeStart = std::chrono::system_clock::now();
		assert(params.graph.finalized);
		auto trace = getBacktraceFullStart(sequence, reusableState);
		auto timeEnd = std::chrono::system_clock::now();
		size_t time = std::chrono::duration_cast<std::chrono::milliseconds>(timeEnd - timeStart).count();
		//failed alignment, don't output
		if (trace.score == std::numeric_limits<ScoreType>::max()) return result;
		if (trace.trace.size() == 0) return result;
#ifndef NDEBUG
		if (trace.trace.size() > 0) verifyTrace(trace.trace, sequence, trace.score);
#endif
		fixForwardTraceSeqPos(trace.trace, 0, sequence);

		AlignmentResult::AlignmentItem alnItem { std::move(trace), 0, std::numeric_limits<size_t>::max() };

		alnItem.alignmentStart = alnItem.trace->trace[0].DPposition.seqPos;
		alnItem.alignmentEnd = alnItem.trace->trace.back().DPposition.seqPos;
		timeEnd = std::chrono::system_clock::now();
		time = std::chrono::duration_cast<std::chrono::milliseconds>(timeEnd - timeStart).count();
		alnItem.elapsedMilliseconds = time;
		result.alignments.emplace_back(std::move(alnItem));
		return result;
	}

	AlignmentResult AlignOneWay(const std::string& seq_id, const std::string& sequence, const std::vector<SeedHit>& seedHits, AlignerGraphsizedState& reusableState) const
	{
		assert(params.graph.finalized);
		AlignmentResult result;
		result.readName = seq_id;
		assert(seedHits.size() > 0);
		// std::vector<std::tuple<size_t, size_t, size_t>> triedAlignmentNodes;
		for (size_t i = 0; i < seedHits.size(); i++)
		{
			std::string seedInfo = std::to_string(seedHits[i].nodeID) + (seedHits[i].reverse ? "-" : "+") + "," + std::to_string(seedHits[i].seqPos) + "," + std::to_string(seedHits[i].matchLen) + "," + std::to_string(seedHits[i].nodeOffset);
			logger << seq_id << " seed " << i << "/" << seedHits.size() << " " << seedInfo;
			assertSetRead(seq_id, seedInfo);
			if (params.sloppyOptimizations)
			{
				bool found = false;
				for (const auto& aln : result.alignments)
				{
					if (aln.alignmentStart <= seedHits[i].seqPos && aln.alignmentEnd >= seedHits[i].seqPos)
					{
						logger << " skipped";
						logger << BufferedWriter::Flush;
						found = true;
						break;
					}
				}
				if (found) continue;
			}
			logger << BufferedWriter::Flush;
			result.seedsExtended += 1;
			auto item = getAlignmentFromSeed(seq_id, sequence, seedHits[i], reusableState);
			if (item.alignmentFailed()) continue;
			result.alignments.emplace_back(std::move(item));
		}
		assertSetRead(seq_id, "No seed");

		return result;
	}

	void AddAlignment(const std::string& seq_id, const std::string& sequence, AlignmentResult::AlignmentItem& alignment)
	{
		assert(alignment.trace->trace.size() > 0);
		auto vgAln = VGAlignment::traceToAlignment(seq_id, sequence, alignment.trace->score, alignment.trace->trace, 0, false);
		alignment.alignment = vgAln;
		alignment.alignment->set_sequence(sequence.substr(alignment.alignmentStart, alignment.alignmentEnd - alignment.alignmentStart));
		alignment.alignment->set_query_position(alignment.alignmentStart);
	}

	void AddCorrected(AlignmentResult::AlignmentItem& alignment)
	{
		assert(alignment.trace != nullptr);
		assert(alignment.trace->trace.size() > 0);
		alignment.corrected.reserve(alignment.trace->trace.back().DPposition.seqPos - alignment.trace->trace[0].DPposition.seqPos);
		alignment.corrected = alignment.trace->trace[0].graphCharacter;
		for (size_t i = 1; i < alignment.trace->trace.size(); i++)
		{
			if (!alignment.trace->trace[i-1].nodeSwitch && alignment.trace->trace[i].DPposition.nodeOffset == alignment.trace->trace[i-1].DPposition.nodeOffset && alignment.trace->trace[i].DPposition.node == alignment.trace->trace[i-1].DPposition.node) continue;
			alignment.corrected += alignment.trace->trace[i].graphCharacter;
		}
	}

private:

	OnewayTrace getBacktraceFullStart(const std::string& sequence, AlignerGraphsizedState& reusableState) const
	{
		return bvAligner.getBacktraceFullStart(sequence, params.forceGlobal, reusableState);
	}

	Trace getTwoDirectionalTrace(const std::string& sequence, SeedHit seedHit, AlignerGraphsizedState& reusableState) const
	{
		assert(seedHit.seqPos >= 0);
		assert(seedHit.seqPos < sequence.size());
		int forwardNodeId;
		int backwardNodeId;
		if (seedHit.reverse)
		{
			forwardNodeId = seedHit.nodeID * 2 + 1;
			backwardNodeId = seedHit.nodeID * 2;
		}
		else
		{
			forwardNodeId = seedHit.nodeID * 2;
			backwardNodeId = seedHit.nodeID * 2 + 1;
		}
		Trace result;
		result.backward.score = std::numeric_limits<ScoreType>::max();
		result.forward.score = std::numeric_limits<ScoreType>::max();
		if (seedHit.seqPos > 0)
		{
			auto backwardPart = CommonUtils::ReverseComplement(sequence.substr(0, seedHit.seqPos));
			auto reversePos = params.graph.GetReversePosition(forwardNodeId, seedHit.nodeOffset);
			assert(reversePos.first == backwardNodeId);
			result.backward = bvAligner.getReverseTraceFromSeed(backwardPart, backwardNodeId, reversePos.second, params.forceGlobal, reusableState);
		}
		if (seedHit.seqPos < sequence.size()-1)
		{
			auto forwardPart = sequence.substr(seedHit.seqPos+1);
			size_t offset = seedHit.nodeOffset;
			result.forward = bvAligner.getReverseTraceFromSeed(forwardPart, forwardNodeId, offset, params.forceGlobal, reusableState);
		}

		if (!result.backward.failed())
		{
			auto reversePos = params.graph.GetReversePosition(forwardNodeId, seedHit.nodeOffset);
			assert(result.backward.trace.back().DPposition.seqPos == (size_t)-1 && params.graph.nodeIDs[result.backward.trace.back().DPposition.node] == backwardNodeId && params.graph.nodeOffset[result.backward.trace.back().DPposition.node] + result.backward.trace.back().DPposition.nodeOffset == reversePos.second);
			std::reverse(result.backward.trace.begin(), result.backward.trace.end());
		}
		if (!result.forward.failed())
		{
			assert(result.forward.trace.back().DPposition.seqPos == (size_t)-1 && params.graph.nodeIDs[result.forward.trace.back().DPposition.node] == forwardNodeId && params.graph.nodeOffset[result.forward.trace.back().DPposition.node] + result.forward.trace.back().DPposition.nodeOffset == seedHit.nodeOffset);
			std::reverse(result.forward.trace.begin(), result.forward.trace.end());
		}
		return result;
	}

	void fixForwardTraceSeqPos(std::vector<TraceItem>& trace, LengthType start, const std::string& sequence) const
	{
		if (trace.size() == 0) return;
		for (size_t i = 0; i < trace.size(); i++)
		{
			trace[i].DPposition.seqPos += start;
			auto nodeIndex = trace[i].DPposition.node;
			trace[i].DPposition.node = params.graph.nodeIDs[nodeIndex];
			trace[i].DPposition.nodeOffset += params.graph.nodeOffset[nodeIndex];
			assert(trace[i].DPposition.seqPos < sequence.size());
			assert(i == 0 || trace[i].DPposition.seqPos == trace[0].DPposition.seqPos || trace[i].sequenceCharacter == sequence[trace[i].DPposition.seqPos]);
		}
		trace[0].sequenceCharacter = sequence[trace[0].DPposition.seqPos];
	}

	void fixReverseTraceSeqPosAndOrder(std::vector<TraceItem>& trace, LengthType end, const std::string& sequence) const
	{
		if (trace.size() == 0) return;
		std::reverse(trace.begin(), trace.end());
		for (size_t i = 0; i < trace.size(); i++)
		{
			assert(trace[i].DPposition.seqPos <= end || trace[i].DPposition.seqPos == (size_t)-1);
			trace[i].DPposition.seqPos = end - trace[i].DPposition.seqPos;
			size_t offset = params.graph.nodeOffset[trace[i].DPposition.node] + trace[i].DPposition.nodeOffset;
			auto reversePos = params.graph.GetReversePosition(params.graph.nodeIDs[trace[i].DPposition.node], offset);
			assert(reversePos.second < params.graph.originalNodeSize.at(params.graph.nodeIDs[trace[i].DPposition.node]));
			trace[i].DPposition.node = reversePos.first;
			trace[i].DPposition.nodeOffset = reversePos.second;
			assert(trace[i].DPposition.seqPos < sequence.size());
			trace[i].sequenceCharacter = sequence[trace[i].DPposition.seqPos];
			trace[i].graphCharacter = CommonUtils::Complement(trace[i].graphCharacter);
		}
		for (size_t i = 0; i < trace.size() - 1; i++)
		{
			trace[i].nodeSwitch = trace[i+1].nodeSwitch;
		}
		trace.back().nodeSwitch = false;
	}

	AlignmentResult::AlignmentItem getAlignmentFromSeed(const std::string& seq_id, const std::string& sequence, SeedHit seedHit, AlignerGraphsizedState& reusableState) const
	{
		assert(params.graph.finalized);
		auto timeStart = std::chrono::system_clock::now();

		auto trace = getTwoDirectionalTrace(sequence, seedHit, reusableState);

#ifndef NDEBUG
		if (trace.forward.trace.size() > 0) verifyTrace(trace.forward.trace, sequence, trace.forward.score);
		if (trace.backward.trace.size() > 0) verifyTrace(trace.backward.trace, sequence, trace.backward.score);
#endif
		fixReverseTraceSeqPosAndOrder(trace.backward.trace, seedHit.seqPos-1, sequence);
		fixForwardTraceSeqPos(trace.forward.trace, seedHit.seqPos+1, sequence);

		//failed alignment, don't output
		if (trace.forward.failed() && trace.backward.failed())
		{
			return emptyAlignment(0, 0);
		}

		// auto traceVector = getTraceInfo(sequence, trace.backward.trace, trace.forward.trace);

		assert(trace.backward.trace.size() > 0 || trace.backward.failed());
		auto mergedTrace = std::move(trace.backward);
		if (mergedTrace.failed())
		{
			mergedTrace = std::move(trace.forward);
		}
		else if (!mergedTrace.failed() && !trace.forward.failed())
		{
			assert(mergedTrace.trace.size() > 0);
			assert(mergedTrace.trace.back().DPposition == trace.forward.trace[0].DPposition);
			mergedTrace.trace.pop_back();
			mergedTrace.trace.insert(mergedTrace.trace.end(), trace.forward.trace.begin(), trace.forward.trace.end());
			mergedTrace.score += trace.forward.score;
		}
		else if (!trace.forward.failed())
		{
			assert(trace.forward.trace.size() > 0);
			mergedTrace.trace.insert(mergedTrace.trace.end(), trace.forward.trace.begin(), trace.forward.trace.end());
			mergedTrace.score += trace.forward.score;
		}

		AlignmentResult::AlignmentItem result { std::move(mergedTrace), 0, std::numeric_limits<size_t>::max() };

		LengthType seqstart = 0;
		LengthType seqend = 0;
		assert(result.trace->trace.size() > 0);
		seqstart = result.trace->trace[0].DPposition.seqPos;
		seqend = result.trace->trace.back().DPposition.seqPos;
		assert(seqend < sequence.size());
		// result.trace = traceVector;
		result.alignmentStart = seqstart;
		result.alignmentEnd = seqend + 1;
		auto timeEnd = std::chrono::system_clock::now();
		size_t time = std::chrono::duration_cast<std::chrono::milliseconds>(timeEnd - timeStart).count();
		result.elapsedMilliseconds = time;
		return result;
	}

	static AlignmentResult::AlignmentItem emptyAlignment(size_t elapsedMilliseconds, size_t cellsProcessed)
	{
		AlignmentResult::AlignmentItem result;
		result.cellsProcessed = cellsProcessed;
		result.elapsedMilliseconds = elapsedMilliseconds;
		return result;
	}

#ifndef NDEBUG
	void verifyTrace(const std::vector<TraceItem>& trace, const std::string& sequence, ScoreType score) const
	{
		size_t start = 0;
		while (trace[start].DPposition.seqPos == (size_t)-1)
		{
			start++;
			assert(start < trace.size());
		}
		start++;
		for (size_t i = start; i < trace.size(); i++)
		{
			assert(trace[i].DPposition.seqPos < sequence.size());
			auto newpos = trace[i].DPposition;
			auto oldpos = trace[i-1].DPposition;
			auto oldNodeIndex = oldpos.node;
			auto newNodeIndex = newpos.node;
			assert(newpos.seqPos != oldpos.seqPos || newpos.node != oldpos.node || newpos.nodeOffset != oldpos.nodeOffset);
			if (oldNodeIndex == newNodeIndex)
			{
				assert((newpos.nodeOffset >= oldpos.nodeOffset && newpos.seqPos >= oldpos.seqPos) || (oldpos.nodeOffset == params.graph.NodeLength(newNodeIndex)-1 && newpos.nodeOffset == 0 && (newpos.seqPos == oldpos.seqPos || newpos.seqPos == oldpos.seqPos+1)));
				continue;
			}
			assert(oldNodeIndex != newNodeIndex);
			assert(std::find(params.graph.outNeighbors[oldpos.node].begin(), params.graph.outNeighbors[oldpos.node].end(), newpos.node) != params.graph.outNeighbors[oldpos.node].end());
			assert(newpos.seqPos == oldpos.seqPos || newpos.seqPos == oldpos.seqPos+1);
			assert(oldpos.nodeOffset == params.graph.NodeLength(oldNodeIndex)-1);
			assert(newpos.nodeOffset == 0);
		}
	}
#endif

};

#endif