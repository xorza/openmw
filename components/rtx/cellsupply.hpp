#pragma once

#include <memory>
#include <stop_token>
#include <vector>

#include <osg/Vec2i>

#include "monitor.hpp"
#include "residency.hpp"
#include "worker.hpp"

namespace Rtx
{
    class CellReader;
    struct PreparedCell;
    struct PreparedModel;
    struct PreparedTexture;

    /// What the reading thread is to read next: the cells, and what to read of each. One value,
    /// because a list that reaches the thread beside the other switch is a cell read under a rule
    /// nobody asked for.
    struct CellRequest
    {
        std::vector<osg::Vec2i> mCells;

        /// Whether to read each cell's references at all, which is the ring's statics switch as it
        /// stood when the ask was made.
        bool mStatics = true;

        bool operator==(const CellRequest& other) const = default;

        bool empty() const { return mCells.empty(); }

        /// Empties the list and keeps the room it grew. The switch is left as it was, because a
        /// request with no cells in it says nothing about either.
        void clear() { mCells.clear(); }

        /// Takes what `from` holds, leaving it empty and keeping the room both grew.
        void take(CellRequest& from);
    };

    /// What the frame has finished with, on its way back to the reader: a cell, the models it
    /// named and the images its ground named leave together.
    struct CellReturns
    {
        std::vector<PreparedCell*> mCells;
        std::vector<PreparedModel*> mModels;
        std::vector<PreparedTexture*> mTextures;

        bool empty() const { return mCells.empty() && mModels.empty() && mTextures.empty(); }

        /// Empties all three, keeping the room each grew.
        void clear();

        /// Appends everything `from` holds and empties it.
        void take(CellReturns& from);
    };

    /// Cells read ahead of the eye, on a thread of its own: the thread, the reader it drives, and
    /// the two channels between them — what to read next, and what the frame has finished with. A
    /// newer ask replaces an older one it has not finished, because the ones the ring lacked a
    /// moment ago are not an answer to what it lacks now. Everything read is lent and given back;
    /// `Spares` says why an address and not a shared count.
    class CellSupply
    {
    public:
        CellSupply();

        /// Stops the thread. A cell in flight is finished and dropped rather than waited out.
        ~CellSupply();

        /// Whether this is already reading `world`, so a caller need not let go of what it holds.
        bool isReading(const CellWorld& world) const { return mWorld == world; }

        /// Whether there is a reader at all, which a world with no content answers no to.
        bool hasReader() const { return mReader != nullptr; }

        /// Points it at `world`, stopping and joining whatever it was reading and forgetting both
        /// channels. The caller lets go of what it held first, or a hold handed back would reach a
        /// reader that never lent it.
        void follow(const CellWorld& world);

        /// Hands the thread `request`. Costs nothing where it equals the last one handed over.
        void ask(const CellRequest& request);

        /// Moves what the thread has read into `into`, appended. Empty where it has read nothing.
        void take(std::vector<PreparedCell*>& into);

        /// Blocks until the thread has read at least one more cell, for a settled run
        /// (`CellRing::setSettled`). False where the reader threw, or a caller would wait for ever.
        bool waitForOne();

        /// Where a caller puts what it has finished with. Handed over by `publish`.
        CellReturns& giveBack()
        {
            mOnFrame.check();
            return mReturning;
        }

        /// Hands the thread everything `giveBack` collected. Nothing where there is none.
        void publish();

    private:
        /// The reader's loop: a list at a time, until asked to stop. `Monitor::serve` is the loop,
        /// and this is what it takes and what it does.
        void work(std::stop_token stop);

        /// Reads the list `work` took, a cell at a time, and stops at the first sign of a newer
        /// one. On the thread, outside the lock but for what it hands over.
        void read(std::stop_token stop);

        /// Gives the reader what the frame gave back. On the thread, under the lock.
        void recycle();

        CellWorld mWorld;

        /// Which thread the two below belong to. The frame's, and `mOnFrame.check()` is what
        /// says so at each of the calls that touch them.
        OwnedBy mOnFrame;

        /// The frame's side: the last ask handed over, and what it is collecting to hand back.
        CellRequest mRequested;
        CellReturns mReturning;

        /// The lock between the frame and the reader, and the two waits across it. It guards the
        /// three below and nothing else.
        Monitor mMonitor;

        /// What the thread is to read next, written whole under the lock and taken whole by the
        /// thread. A newer request replaces an older one it has not finished.
        CellRequest mWanted;

        /// What the thread has read, under the lock.
        std::vector<PreparedCell*> mDone;

        /// What the frame has given back, under the lock, for the thread to refill.
        CellReturns mReturned;

        /// The thread's own: the request it is working through.
        CellRequest mReading;

        /// Owned here and used by the thread alone while it runs.
        std::unique_ptr<CellReader> mReader;

        /// Last, for the reason `Worker` gives.
        Worker mWorker;
    };
}
