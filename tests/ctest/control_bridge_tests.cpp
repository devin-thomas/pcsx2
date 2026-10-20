// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "ControlBridgeProtocol.h"
#include <gtest/gtest.h>

using namespace ControlBridge;

namespace
{
	class FakeEnvironment final : public Environment
	{
	public:
		bool paused = true;
		bool executing = false;
		bool pad = true;
		bool conflict = false;
		uint16_t mask = 0;
		uint64_t clock = 100;
		int reads = 0;
		int syncs = 0;
		bool Paused() const override { return paused; }
		bool Executing() const override { return executing; }
		bool SupportedPad() const override { return pad; }
		bool Conflict() const override { return conflict; }
		bool Neutral() const override { return mask == 0; }
		uint64_t Now() const override { return clock; }
		void Apply(uint16_t value) override
		{
			mask = value;
			++clock;
		}
		void Resume() override { paused = false; }
		void Pause() override { paused = true; }
		void Synchronize() override
		{
			++syncs;
			++clock;
		}
		void Read(const Range& range, std::vector<uint8_t>& data) override
		{
			EXPECT_TRUE(paused);
			EXPECT_FALSE(executing);
			EXPECT_GT(syncs, 0);
			++reads;
			data.insert(data.end(), range.length, uint8_t(range.address));
		}
	};
	class BridgeTest : public testing::Test
	{
	public:
		State state;
		FakeEnvironment env;
		uint64_t id = 0;
		uint64_t epoch = 1;
		Receipt Send(Command command, uint32_t count = 0, uint32_t mask = 0)
		{
			Request request;
			request.command = command;
			request.id = ++id;
			request.epoch = command == Command::Hello ? 0 : epoch;
			request.count = count;
			request.mask = mask;
			Receipt result = state.Handle(42, request, env, env.clock);
			epoch = result.epoch;
			return result;
		}
		void Acquire()
		{
			ASSERT_EQ(Send(Command::Hello).status, Status::Ok);
			ASSERT_EQ(Send(Command::Acquire).status, Status::Ok);
		}
		void Boundary()
		{
			env.executing = true;
			++env.clock;
			state.Boundary(env, env.clock);
		}
		void Stopped()
		{
			env.executing = false;
			state.ExecutionStopped(env, env.clock);
		}
	};

	TEST_F(BridgeTest, ExactCountsAndCompletionAfterExecutionUnwinds)
	{
		for (uint32_t count : {1u, 2u, 17u, 300u})
		{
			if (!state.Leased())
				Acquire();
			const auto started = Send(Command::Step, count, 0x4081);
			ASSERT_EQ(started.status, Status::Pending);
			ASSERT_TRUE(started.applied_valid);
			EXPECT_FALSE(started.completed_valid);
			EXPECT_EQ(started.applied_ns, env.clock);
			EXPECT_EQ(env.mask, 0x4081);
			for (uint32_t boundary = 0; boundary < count; ++boundary)
			{
				Boundary();
				if (boundary + 1 < count)
				{
					EXPECT_FALSE(env.paused);
					EXPECT_EQ(env.mask, 0x4081);
				}
			}
			ASSERT_TRUE(env.paused);
			ASSERT_EQ(env.mask, 0);
			const auto early = Send(Command::Query);
			EXPECT_EQ(early.action_status, Status::Pending);
			EXPECT_FALSE(early.completed_valid);
			Stopped();
			const auto done = Send(Command::Query);
			EXPECT_EQ(done.action_status, Status::Ok);
			EXPECT_TRUE(done.completed_valid);
			EXPECT_EQ(done.completed_count, count);
			EXPECT_EQ(done.completed - done.applied, count);
			EXPECT_GT(done.completed_ns, done.applied_ns);
			EXPECT_EQ(done.action, started.request);
			EXPECT_EQ(done.action_epoch, started.epoch);
			EXPECT_EQ(done.executed_mask, 0x4081);
			EXPECT_EQ(done.released_mask, 0);
		}
	}
	TEST_F(BridgeTest, RequiresHandshakeAndPausedNeutralP1)
	{
		EXPECT_EQ(Send(Command::Acquire).status, Status::Handshake);
		ASSERT_EQ(Send(Command::Hello).status, Status::Ok);
		env.paused = false;
		EXPECT_EQ(Send(Command::Acquire).status, Status::VMState);
		env.paused = true;
		env.executing = true;
		EXPECT_EQ(Send(Command::Acquire).status, Status::VMState);
		env.executing = false;
		env.mask = 1;
		EXPECT_EQ(Send(Command::Acquire).status, Status::NotNeutral);
		env.mask = 0;
		env.pad = false;
		EXPECT_EQ(Send(Command::Acquire).status, Status::Pad);
	}
	TEST_F(BridgeTest, RejectsConflictingInputSource)
	{
		ASSERT_EQ(Send(Command::Hello).status, Status::Ok);
		env.conflict = true;
		EXPECT_EQ(Send(Command::Acquire).status, Status::Conflict);
		EXPECT_FALSE(state.Leased());
	}
	TEST_F(BridgeTest, RejectsInvalidCountsAndMasks)
	{
		for (auto pair : {std::pair{0u, 0u}, {301u, 0u}, {1u, 0x10000u}})
		{
			State clean;
			state = clean;
			Acquire();
			EXPECT_EQ(Send(Command::Step, pair.first, pair.second).status, Status::Bounds);
			EXPECT_EQ(env.mask, 0);
			EXPECT_TRUE(env.paused);
		}
	}
	TEST_F(BridgeTest, DuplicateIDsInvalidateRatherThanReplay)
	{
		Acquire();
		const auto started = Send(Command::Step, 10, 1);
		Request duplicate;
		duplicate.command = Command::Step;
		duplicate.epoch = epoch;
		duplicate.id = started.request;
		duplicate.count = 10;
		const auto result = state.Handle(42, duplicate, env, env.clock);
		EXPECT_EQ(result.status, Status::RequestID);
		EXPECT_GT(result.epoch, epoch);
		EXPECT_EQ(env.mask, 0);
		EXPECT_TRUE(env.paused);
		EXPECT_FALSE(result.completed_valid);
	}
	TEST_F(BridgeTest, RejectsStaleEpochUnsupportedVersionAndPort)
	{
		Acquire();
		Request request;
		request.id = ++id;
		request.epoch = epoch - 1;
		request.command = Command::Step;
		request.count = 1;
		EXPECT_EQ(state.Handle(42, request, env, env.clock).status, Status::Epoch);
		request.version = 99;
		EXPECT_EQ(state.Handle(42, request, env, env.clock).status, Status::Version);
		request.version = VERSION;
		request.port = 1;
		EXPECT_EQ(state.Handle(42, request, env, env.clock).status, Status::Pad);
		request.port = 0;
		request.command = static_cast<Command>(255);
		EXPECT_EQ(state.Handle(42, request, env, env.clock).status, Status::Unsupported);
	}
	TEST_F(BridgeTest, OnlyOneInFlightAction)
	{
		Acquire();
		Send(Command::Step, 20, 0x40);
		EXPECT_EQ(Send(Command::Step, 1, 1).status, Status::Busy);
		EXPECT_EQ(env.mask, 0);
		EXPECT_TRUE(env.paused);
	}
	TEST_F(BridgeTest, CancelPreservesPartialCountButInvalidatesCompletion)
	{
		Acquire();
		const auto started = Send(Command::Step, 20, 0x80);
		Boundary();
		const auto cancelled = Send(Command::Cancel);
		EXPECT_EQ(cancelled.status, Status::Cancelled);
		EXPECT_EQ(cancelled.completed_count, 1);
		EXPECT_EQ(cancelled.action, started.action);
		EXPECT_EQ(cancelled.action_epoch, started.epoch);
		EXPECT_NE(cancelled.epoch, cancelled.action_epoch);
		EXPECT_FALSE(cancelled.completed_valid);
		EXPECT_FALSE(state.Leased());
		EXPECT_TRUE(env.paused);
		EXPECT_EQ(env.mask, 0);
	}
	TEST_F(BridgeTest, TimeoutDisconnectAndResetNeutralize)
	{
		for (auto reason : {Status::Timeout, Status::Disconnected, Status::Invalidated})
		{
			state = State();
			Acquire();
			Send(Command::Step, 20, 0xffff);
			Boundary();
			if (reason == Status::Timeout)
				state.Tick(env, env.clock + LEASE_NS);
			else if (reason == Status::Disconnected)
				state.Disconnect(42, env);
			else
				state.Abort(env, reason);
			EXPECT_FALSE(state.Leased());
			EXPECT_FALSE(state.Active());
			EXPECT_EQ(env.mask, 0);
			EXPECT_TRUE(env.paused);
			Stopped();
			const auto failed = Send(Command::Query);
			EXPECT_EQ(failed.action_status, reason);
			EXPECT_FALSE(failed.completed_valid);
			EXPECT_EQ(failed.completed_count, 1);
		}
	}
	TEST_F(BridgeTest, ForeignDisconnectCannotCancelOwner)
	{
		Acquire();
		Send(Command::Step, 2, 1);
		state.Disconnect(99, env);
		EXPECT_TRUE(state.Active());
		EXPECT_EQ(env.mask, 1);
	}
	TEST_F(BridgeTest, ForeignRequestsCannotChangeOwnerActionOrIDs)
	{
		Acquire();
		const auto started = Send(Command::Step, 10, 0x4081);
		const auto before = Send(Command::Query);
		for (auto command : {Command::Hello, Command::Query, Command::Step, Command::Cancel, Command::Release})
		{
			for (unsigned invalid = 0; invalid < 4; ++invalid)
			{
				Request request;
				request.command = command;
				request.epoch = command == Command::Hello ? 0 : epoch;
				request.id = 1000000;
				if (invalid == 1)
					request.version = 999;
				else if (invalid == 2)
					request.port = 1;
				else if (invalid == 3)
					request.command = static_cast<Command>(255);
				const auto rejected = state.Handle(99, request, env, env.clock);
				EXPECT_NE(rejected.status, Status::Ok);
				EXPECT_NE(rejected.status, Status::Pending);
				ASSERT_TRUE(state.Leased());
				ASSERT_TRUE(state.Active());
				EXPECT_FALSE(env.paused);
				EXPECT_EQ(env.mask, 0x4081);
				const auto after = Send(Command::Query);
				EXPECT_EQ(after.status, Status::Ok);
				EXPECT_EQ(after.action_status, Status::Pending);
				EXPECT_EQ(after.epoch, before.epoch);
				EXPECT_EQ(after.action_epoch, before.action_epoch);
				EXPECT_EQ(after.action, started.request);
				EXPECT_EQ(after.current, before.current);
				EXPECT_EQ(after.applied, before.applied);
				EXPECT_EQ(after.applied_ns, before.applied_ns);
				EXPECT_EQ(after.completed_ns, before.completed_ns);
				EXPECT_EQ(after.completed_count, 0);
				EXPECT_EQ(after.requested_count, 10);
				EXPECT_EQ(after.executed_mask, 0x4081);
				EXPECT_TRUE(after.applied_valid);
				EXPECT_FALSE(after.completed_valid);
			}
		}
	}
	TEST_F(BridgeTest, OwnerVersionPortAndCommandErrorsStillNeutralize)
	{
		for (unsigned invalid = 0; invalid < 3; ++invalid)
		{
			state = State();
			Acquire();
			const auto started = Send(Command::Step, 10, 0x40);
			Request request;
			request.id = ++id;
			request.epoch = epoch;
			request.command = Command::Query;
			if (invalid == 0)
				request.version = 999;
			else if (invalid == 1)
				request.port = 1;
			else
				request.command = static_cast<Command>(255);
			const auto result = state.Handle(42, request, env, env.clock);
			EXPECT_FALSE(state.Leased());
			EXPECT_FALSE(state.Active());
			EXPECT_TRUE(env.paused);
			EXPECT_EQ(env.mask, 0);
			EXPECT_FALSE(result.completed_valid);
			EXPECT_EQ(result.action, started.request);
			EXPECT_EQ(result.action_epoch, started.epoch);
			EXPECT_GT(result.epoch, result.action_epoch);
		}
	}
	TEST_F(BridgeTest, ForeignRequestCannotPreventAnExpiredDeadline)
	{
		Acquire();
		const auto started = Send(Command::Step, 10, 0x40);
		Request request;
		request.version = 999;
		const auto result = state.Handle(99, request, env, env.clock + LEASE_NS);
		EXPECT_EQ(result.status, Status::Version);
		EXPECT_EQ(result.action_status, Status::Timeout);
		EXPECT_EQ(result.action, started.request);
		EXPECT_FALSE(result.completed_valid);
		EXPECT_FALSE(state.Leased());
		EXPECT_TRUE(env.paused);
		EXPECT_EQ(env.mask, 0);
	}
	TEST_F(BridgeTest, UnexpectedExecutionStopCannotBecomeSuccess)
	{
		Acquire();
		Send(Command::Step, 2, 1);
		Boundary();
		Stopped();
		EXPECT_FALSE(state.Leased());
		EXPECT_EQ(env.mask, 0);
		const auto result = Send(Command::Query);
		EXPECT_EQ(result.action_status, Status::Invalidated);
		EXPECT_FALSE(result.completed_valid);
	}
	TEST_F(BridgeTest, SnapshotValidatesEntireListBeforeReading)
	{
		Acquire();
		Send(Command::Step, 1);
		Boundary();
		Stopped();
		Request request;
		request.command = Command::Snapshot;
		request.id = ++id;
		request.epoch = epoch;
		request.ranges = {{7, 3}, {0x1ffffff, 1}};
		const auto result = state.Handle(42, request, env, env.clock);
		EXPECT_EQ(result.status, Status::Ok);
		EXPECT_EQ(result.data, (std::vector<uint8_t>{7, 7, 7, 255}));
		EXPECT_EQ(env.reads, 2);
		request.id = ++id;
		request.ranges = {{0, 1}, {0x1ffffff, 2}};
		EXPECT_EQ(state.Handle(42, request, env, env.clock).status, Status::Bounds);
		EXPECT_EQ(env.reads, 2);
	}
	TEST_F(BridgeTest, SnapshotRejectsOversizedListAndBytes)
	{
		for (const auto& ranges : {std::vector<Range>(33, {0, 1}), std::vector<Range>{{0, 4096}, {1, 1}},
				 std::vector<Range>{{0xffffffff, 1}}, std::vector<Range>{{0, 0}}})
		{
			state = State();
			Acquire();
			Send(Command::Step, 1);
			Boundary();
			Stopped();
			Request request;
			request.command = Command::Snapshot;
			request.id = ++id;
			request.epoch = epoch;
			request.ranges = ranges;
			EXPECT_EQ(state.Handle(42, request, env, env.clock).status, Status::Bounds);
			EXPECT_EQ(env.reads, 0);
		}
	}
	TEST_F(BridgeTest, ReleaseLeavesPausedNeutralAndChangesEpoch)
	{
		Acquire();
		const auto released = Send(Command::Release);
		EXPECT_EQ(released.status, Status::Ok);
		EXPECT_FALSE(released.leased);
		EXPECT_TRUE(env.paused);
		EXPECT_EQ(env.mask, 0);
		EXPECT_GT(released.epoch, 2);
	}
	TEST_F(BridgeTest, ResetWithoutLeaseStillInvalidatesEpoch)
	{
		Send(Command::Hello);
		const auto previous = epoch;
		state.Invalidate(env);
		const auto result = Send(Command::Acquire);
		EXPECT_EQ(result.status, Status::Epoch);
		EXPECT_GT(result.epoch, previous);
		EXPECT_FALSE(result.completed_valid);
	}
	TEST_F(BridgeTest, SnapshotRequiresCompletedControlBoundary)
	{
		Acquire();
		Request request;
		request.command = Command::Snapshot;
		request.id = ++id;
		request.epoch = epoch;
		request.ranges = {{0, 4}};
		EXPECT_EQ(state.Handle(42, request, env, env.clock).status, Status::Busy);
		EXPECT_EQ(env.reads, 0);
	}
	TEST_F(BridgeTest, NewlyConflictingSourceCancelsActiveInput)
	{
		Acquire();
		Send(Command::Step, 10, 0x40);
		env.conflict = true;
		const auto result = Send(Command::Query);
		EXPECT_EQ(result.status, Status::Conflict);
		EXPECT_FALSE(result.completed_valid);
		EXPECT_FALSE(state.Active());
		EXPECT_TRUE(env.paused);
		EXPECT_EQ(env.mask, 0);
	}
	TEST_F(BridgeTest, DeadlineAtFinalBoundaryCannotPublishSuccess)
	{
		Acquire();
		Send(Command::Step, 1, 0x40);
		env.clock += LEASE_NS;
		Boundary();
		Stopped();
		const auto result = Send(Command::Query);
		EXPECT_EQ(result.action_status, Status::Timeout);
		EXPECT_FALSE(result.completed_valid);
		EXPECT_EQ(result.completed_count, 1);
		EXPECT_TRUE(env.paused);
		EXPECT_EQ(env.mask, 0);
	}
	TEST_F(BridgeTest, DelayedExecutionUnwindCannotPublishAfterDeadline)
	{
		Acquire();
		Send(Command::Step, 1, 0x40);
		Boundary();
		env.clock += LEASE_NS;
		Stopped();
		const auto result = Send(Command::Query);
		EXPECT_EQ(result.action_status, Status::Timeout);
		EXPECT_FALSE(result.completed_valid);
		EXPECT_EQ(result.completed_count, 1);
		EXPECT_TRUE(env.paused);
		EXPECT_EQ(env.mask, 0);
	}
	TEST(BridgeProtocol, DecodesUnalignedLittleEndianAndRejectsLengths)
	{
		std::vector<uint8_t> frame{0x10};
		Append(frame, VERSION);
		Append(frame, uint8_t(Command::Snapshot));
		Append(frame, uint8_t(0));
		Append(frame, uint64_t(0x0123456789abcdef));
		Append(frame, uint64_t(1234));
		Append(frame, uint32_t(0));
		Append(frame, uint32_t(0));
		Append(frame, uint16_t(1));
		Append(frame, uint16_t(0));
		Append(frame, uint32_t(0x1234567));
		Append(frame, uint32_t(4));
		Request request;
		ASSERT_TRUE(DecodeRequest(std::span(frame).subspan(1), request));
		EXPECT_EQ(request.epoch, 0x0123456789abcdef);
		EXPECT_EQ(request.id, 1234);
		ASSERT_EQ(request.ranges.size(), 1);
		EXPECT_EQ(request.ranges[0].address, 0x1234567);
		for (size_t length = 0; length < frame.size() - 1; ++length)
			EXPECT_FALSE(DecodeRequest(std::span(frame).subspan(1, length), request));
		frame.push_back(0);
		EXPECT_FALSE(DecodeRequest(std::span(frame).subspan(1), request));
		frame.pop_back();
		frame[29] = 33; // Reject range count before allocating the list.
		EXPECT_FALSE(DecodeRequest(std::span(frame).subspan(1), request));
		frame[29] = 1;
		frame[31] = 1;
		EXPECT_FALSE(DecodeRequest(std::span(frame).subspan(1), request));
	}
	TEST(BridgeProtocol, ReplySchemaPreservesUnknownCompletion)
	{
		Receipt receipt;
		receipt.status = Status::Timeout;
		receipt.action_status = Status::Timeout;
		receipt.epoch = 0;
		receipt.request = 0x123456789abcdef;
		receipt.action_epoch = 7654;
		receipt.completed_count = 7;
		const auto bytes = EncodeReply(receipt);
		ASSERT_EQ(bytes.size(), 112);
		EXPECT_EQ(Take<uint16_t>(bytes, 2), uint16_t(Status::Timeout));
		EXPECT_EQ(Take<uint16_t>(bytes, 6), 0);
		EXPECT_EQ(Take<uint64_t>(bytes, 8), 0);
		EXPECT_EQ(Take<uint64_t>(bytes, 16), receipt.request);
		EXPECT_EQ(Take<uint32_t>(bytes, 84), 7);
		EXPECT_EQ(Take<uint32_t>(bytes, 100), 0);
		EXPECT_EQ(Take<uint64_t>(bytes, 104), 7654);
	}
} // namespace

int main(int argc, char** argv)
{
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
