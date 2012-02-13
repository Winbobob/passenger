/*
 *  Phusion Passenger - http://www.modrails.com/
 *  Copyright (c) 2011, 2012 Phusion
 *
 *  "Phusion Passenger" is a trademark of Hongli Lai & Ninh Bui.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 *  THE SOFTWARE.
 */
#ifndef _PASSENGER_APPLICATION_POOL2_GROUP_H_
#define _PASSENGER_APPLICATION_POOL2_GROUP_H_

#include <string>
#include <map>
#include <queue>
#include <deque>
#include <boost/thread.hpp>
#include <boost/bind.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/make_shared.hpp>
#include <oxt/macros.hpp>
#include <oxt/thread.hpp>
#include <cassert>
#include <ApplicationPool2/Common.h>
#include <ApplicationPool2/ComponentInfo.h>
#include <ApplicationPool2/Spawner.h>
#include <ApplicationPool2/Process.h>
#include <ApplicationPool2/Options.h>
#include <Utils/CachedFileStat.hpp>
#include <Utils/FileChangeChecker.h>

namespace Passenger {
namespace ApplicationPool2 {

using namespace std;
using namespace boost;
using namespace oxt;


/**
 * Except for otherwise documented parts, this class is not thread-safe,
 * so only access within ApplicationPool lock.
 */
class Group: public enable_shared_from_this<Group> {
private:
	friend class Pool;
	friend class SuperGroup;
	
	struct GetAction {
		GetCallback callback;
		SessionPtr session;
	};
	
	struct DisableWaiter {
		ProcessPtr process;
		Callback callback;
		
		DisableWaiter(const ProcessPtr &_process, const Callback &_callback)
			: process(_process),
			  callback(_callback)
			{ }
	};
	
	mutable boost::mutex backrefSyncher;
	weak_ptr<SuperGroup> superGroup;
	CachedFileStat cstat;
	FileChangeChecker fileChangeChecker;
	string restartFile;
	string alwaysRestartFile;
	
	
	static void _onSessionInitiateFailure(Session *session) {
		const ProcessPtr &process = session->getProcess();
		GroupPtr group = process->getGroup();
		if (OXT_LIKELY(group != NULL)) {
			group->onSessionInitiateFailure(process, session);
		}
	}

	static void _onSessionClose(Session *session) {
		const ProcessPtr &process = session->getProcess();
		GroupPtr group = process->getGroup();
		if (OXT_LIKELY(group != NULL)) {
			group->onSessionClose(process, session);
		}
	}
	
	void createInterruptableThread(const function<void ()> &func, const string &name,
		unsigned int stackSize);
	string generateSecret() const;
	void onSessionInitiateFailure(const ProcessPtr &process, Session *session);
	void onSessionClose(const ProcessPtr &process, Session *session);
	void spawnThreadMain(GroupPtr self, SpawnerPtr spawner, Options options);
	
	void verifyInvariants() const {
		// !a || b: logical equivalent of a IMPLIES b.
		
		assert(count >= 0);
		assert(0 <= disablingCount && disablingCount <= count);
		assert(processes.empty() == (count == 0));
		assert(processes.empty() == (pqueue.top() == NULL));
		assert(disabledCount >= 0);
		assert(disabledProcesses.empty() == (disabledCount == 0));
		assert(!( count > 0 && disablingCount == count ) || ( spawning() ));
		
		// Verify getWaitlist invariants.
		assert(!( !getWaitlist.empty() ) || ( processes.empty() || pqueue.top()->atFullCapacity() ));
		assert(!( !processes.empty() && !pqueue.top()->atFullCapacity() ) || ( getWaitlist.empty() ));
		assert(!( processes.empty() && !spawning() ) || ( getWaitlist.empty() ));
		assert(!( !getWaitlist.empty() ) || ( !processes.empty() || spawning() ));
		
		// Verify disableWaitlist invariants.
		assert((int) disableWaitlist.size() >= disablingCount);
	}
	
	void resetOptions(const Options &newOptions) {
		options = newOptions;
		options.persist(newOptions);
		options.clearPerRequestFields();
		options.groupSecret = secret;
	}
	
	void mergeOptions(const Options &other) {
		options.maxRequests      = other.maxRequests;
		options.minProcesses     = other.minProcesses;
		options.statThrottleRate = other.statThrottleRate;
		options.spawnerTimeout   = other.spawnerTimeout;
	}
	
	void runAllActions(const vector<Callback> &actions) {
		vector<Callback>::const_iterator it, end = actions.end();
		for (it = actions.begin(); it != end; it++) {
			(*it)();
		}
	}
	
	SessionPtr newSession() {
		assert(count > 0);
		Process *process   = pqueue.top();
		SessionPtr session = process->newSession();
		session->onInitiateFailure = _onSessionInitiateFailure;
		session->onClose   = _onSessionClose;
		pqueue.pop();
		process->pqHandle  = pqueue.push(process, process->usage());
		return session;
	}
	
	template<typename Lock>
	void assignSessionsToGetWaitersQuickly(Lock &lock) {
		#ifndef __clang__
		if (OXT_LIKELY(getWaitlist.size() < 50)) {
			// Clang doesn't support variable-length non-POD arrays.
			GetAction actions[getWaitlist.size()];
			unsigned int count = 0;
			while (!getWaitlist.empty() && pqueue.top() != NULL && !pqueue.top()->atFullCapacity()) {
				actions[count].callback = getWaitlist.front().callback;
				actions[count].session  = newSession();
				getWaitlist.pop();
				count++;
			}
			
			verifyInvariants();
			lock.unlock();
			for (unsigned int i = 0; i < count; i++) {
				actions[i].callback(actions[i].session, ExceptionPtr());
			}
		} else
		#endif /* __clang__ */
		{
			vector<GetAction> actions;
			actions.reserve(getWaitlist.size());
			while (!getWaitlist.empty() && pqueue.top() != NULL && !pqueue.top()->atFullCapacity()) {
				GetAction action;
				action.callback = getWaitlist.front().callback;
				action.session  = newSession();
				getWaitlist.pop();
				actions.push_back(action);
			}
			
			verifyInvariants();
			lock.unlock();
			vector<GetAction>::const_iterator it, end = actions.end();
			for (it = actions.begin(); it != actions.end(); it++) {
				it->callback(it->session, ExceptionPtr());
			}
		}
	}
	
	void assignSessionsToGetWaiters(vector<Callback> &postLockActions) {
		while (!getWaitlist.empty() && pqueue.top() != NULL && !pqueue.top()->atFullCapacity()) {
			postLockActions.push_back(boost::bind(
				getWaitlist.front().callback, newSession(),
				ExceptionPtr()));
			getWaitlist.pop();
		}
	}
	
	void assignExceptionToGetWaiters(const ExceptionPtr &exception,
		vector<Callback> &postLockActions)
	{
		while (!getWaitlist.empty()) {
			postLockActions.push_back(boost::bind(
				getWaitlist.front().callback, SessionPtr(),
				exception));
			getWaitlist.pop();
		}
	}
	
	void removeFromDisableWaitlist(const ProcessPtr &p, vector<Callback> &postLockActions) {
		deque<DisableWaiter>::const_iterator it, end = disableWaitlist.end();
		deque<DisableWaiter> newList;
		for (it = disableWaitlist.begin(); it != disableWaitlist.end(); it++) {
			const DisableWaiter &waiter = *it;
			const ProcessPtr process = waiter.process;
			if (process == p) {
				postLockActions.push_back(waiter.callback);
			} else {
				newList.push_back(waiter);
			}
		}
		disableWaitlist = newList;
	}
	
public:
	Options options;
	string name;
	string secret;
	ComponentInfo componentInfo;
	
	/**
	 * 'processes' contains all enabled processes in this group.
	 * 'disabledProcesses' contains all disabled processes in this group.
	 * They do not intersect.
	 *
	 * 'pqueue' orders all enabled processes according to usage() values, from small to large.
	 * 'count' indicates the total number of enabled processes in this group.
	 * 'disablingCount' indicates the number of processes in 'processes' with enabled == DISABLING.
	 * 'disabledCount' indicates the number of disabled processes.
	 *
	 * Invariants:
	 *    count >= 0
	 *    0 <= disablingCount <= count
	 *    processes.size() == count
	 *    processes.empty() == (pqueue.top() == NULL)
	 *    disabledProcesses.size() == disabledCount
	 *    if pqueue.top().atFullCapacity():
	 *       All enabled processes are at full capacity.
	 *    if (count > 0) and (disablingCount == count):
	 *       spawning()
	 *    for all process in processes:
	 *       process.enabled == Process::ENABLED || process.enabled == Process::DISABLING
	 *    for all process in disabledProcesses:
	 *       process.enabled == Process::DISABLED
	 */
	int count;
	int disablingCount;
	int disabledCount;
	PriorityQueue<Process> pqueue;
	ProcessList processes;
	ProcessList disabledProcesses;
	
	/**
	 * get() requests for this group that cannot be immediately satisfied are
	 * put on this wait list, which must be processed as soon as the necessary
	 * resources have become free.
	 *
	 * Invariant 1:
	 *    if getWaitlist is non-empty:
	 *       processes.empty() or (all enabled processes are at full capacity)
	 * Equivalently:
	 *    if !processes.empty() and (an enabled process is not at full capacity):
	 *        getWaitlist is empty.
	 *
	 * Invariant 2:
	 *    if processes.empty() && !spawning():
	 *       getWaitlist is empty
	 * Equivalently:
	 *    if getWaitlist is non-empty:
	 *       !processes.empty() || spawning()
	 */
	queue<GetWaiter> getWaitlist;
	/**
	 * Invariant:
	 *    disableWaitlist.size() >= disablingCount
	 */
	deque<DisableWaiter> disableWaitlist;
	
	SpawnerPtr spawner;
	bool m_spawning;
	
	Group(const SuperGroupPtr &superGroup, const Options &options, const ComponentInfo &info);
	
	SessionPtr get(const Options &newOptions, const GetCallback &callback) {
		if (OXT_UNLIKELY(needsRestart(newOptions))) {
			restart(newOptions);
		} else {
			mergeOptions(newOptions);
		}
		if (OXT_UNLIKELY(!newOptions.noop && shouldSpawn())) {
			spawn();
		}
		
		if (OXT_UNLIKELY(newOptions.noop)) {
			ProcessPtr process = make_shared<Process>((SafeLibev *) NULL,
				0, string(), string(),
				FileDescriptor(), FileDescriptor(),
				SocketListPtr(), 0);
			process->setGroup(shared_from_this());
			return make_shared<Session>(process, (Socket *) NULL);
		}
		
		if (OXT_UNLIKELY(count == 0)) {
			/* We don't have any processes yet, but it's on the way.
			 * Call the callback after a process has been spawned
			 * or has failed to spawn.
			 */
			assert(spawning());
			getWaitlist.push(GetWaiter(newOptions, callback));
			return SessionPtr();
		} else {
			Process *process = pqueue.top();
			assert(process != NULL);
			if (process->atFullCapacity()) {
				/* Looks like all processes are at full capacity.
				 * Wait until a new one has been spawned or until
				 * resources have become free.
				 */
				getWaitlist.push(GetWaiter(newOptions, callback));
				return SessionPtr();
			} else {
				return newSession();
			}
		}
	}
	
	// Thread-safe.
	SuperGroupPtr getSuperGroup() const {
		lock_guard<boost::mutex> lock(backrefSyncher);
		return superGroup.lock();
	}
	
	// Thread-safe.
	void setSuperGroup(const SuperGroupPtr &superGroup) {
		lock_guard<boost::mutex> lock(backrefSyncher);
		this->superGroup = superGroup;
	}
	
	// Thread-safe.
	PoolPtr getPool() const;
	
	// Thread-safe.
	bool detached() const {
		return getSuperGroup() == NULL;
	}
	
	/**
	 * Attaches the given process to this Group and mark it as enabled. This
	 * function doesn't touch getWaitlist so be sure to fix its invariants
	 * afterwards if necessary.
	 */
	void attach(const ProcessPtr &process, vector<Callback> &postLockActions) {
		assert(process->getGroup() == NULL);
		process->setGroup(shared_from_this());
		processes.push_back(process);
		process->it = processes.last_iterator();
		process->pqHandle = pqueue.push(process.get(), process->usage());
		process->enabled = Process::ENABLED;
		count++;
		
		// Disable all processes in 'disableWaitlist' and call their callbacks
		// outside the lock.
		deque<DisableWaiter>::const_iterator it, end = disableWaitlist.end();
		postLockActions.reserve(postLockActions.size() + disableWaitlist.size());
		for (it = disableWaitlist.begin(); it != disableWaitlist.end(); it++) {
			const DisableWaiter &waiter = *it;
			const ProcessPtr process = waiter.process;
			// The same process can appear multiple times in disableWaitlist.
			assert(process->enabled == Process::DISABLING
				|| process->enabled == Process::DISABLED);
			if (process->enabled == Process::DISABLING) {
				process->enabled = Process::DISABLED;
				processes.erase(process->it);
				pqueue.erase(process->pqHandle);
				disabledProcesses.push_back(process);
				process->it = disabledProcesses.last_iterator();
				count--;
				disablingCount--;
				disabledCount++;
			}
			postLockActions.push_back(waiter.callback);
		}
		disableWaitlist.clear();
	}
	
	/**
	 * Detaches the given process from this Group. This function doesn't touch
	 * getWaitlist so be sure to fix its invariants afterwards if necessary.
	 */
	void detach(const ProcessPtr &process, vector<Callback> &postLockActions) {
		assert(process->getGroup().get() == this);
		if (process->enabled == Process::ENABLED || process->enabled == Process::DISABLING) {
			assert(count > 0);
			process->setGroup(GroupPtr());
			processes.erase(process->it);
			pqueue.erase(process->pqHandle);
			count--;
			if (process->enabled == Process::DISABLING) {
				disablingCount--;
				removeFromDisableWaitlist(process, postLockActions);
			}
		} else {
			assert(!disabledProcesses.empty());
			process->setGroup(GroupPtr());
			disabledProcesses.erase(process->it);
			disabledCount--;
		}
	}
	
	/**
	 * Detaches all processes from this Group. This function doesn't touch
	 * getWaitlist so be sure to fix its invariants afterwards if necessary.
	 */
	void detachAll(vector<Callback> &postLockActions) {
		ProcessList::iterator it, end = processes.end();
		for (it = processes.begin(); it != end; it++) {
			(*it)->setGroup(GroupPtr());
		}
		
		processes.clear();
		disabledProcesses.clear();
		pqueue.clear();
		count = 0;
		disablingCount = 0;
		disabledCount = 0;
		
		postLockActions.reserve(disableWaitlist.size());
		while (!disableWaitlist.empty()) {
			const DisableWaiter &waiter = disableWaitlist.front();
			assert(waiter.process->enabled == Process::DISABLING);
			postLockActions.push_back(waiter.callback);
			disableWaitlist.pop_front();
		}
	}
	
	/**
	 * Marks the given process as enabled. This function doesn't touch getWaitlist
	 * so be sure to fix its invariants afterwards if necessary.
	 */
	void enable(const ProcessPtr &process, vector<Callback> &postLockActions) {
		assert(process->getGroup().get() == this);
		if (process->enabled == Process::DISABLING) {
			process->enabled = Process::ENABLED;
			disablingCount--;
			removeFromDisableWaitlist(process, postLockActions);
		} else if (process->enabled == Process::DISABLED) {
			disabledProcesses.erase(process->it);
			processes.push_back(process);
			process->it = processes.last_iterator();
			process->pqHandle = pqueue.push(process.get(), process->usage());
			process->enabled = Process::ENABLED;
			count++;
			disabledCount--;
		}
	}
	
	/**
	 * Marks the given process as disabled.
	 */
	bool disable(const ProcessPtr &process, const Callback &callback) {
		assert(process->getGroup().get() == this);
		if (process->enabled == Process::ENABLED) {
			assert(count > 0);
			if (count - disablingCount == 1) {
				/* All processes are going to be disabled, so in order
				 * to avoid blocking requests we first spawn a new process
				 * and disable this process after the other one is done
				 * spawning. We do this irregardless of resource limits
				 * because we assume the administrator knows what he's
				 * doing.
				 */
				process->enabled = Process::DISABLING;
				disablingCount++;
				disableWaitlist.push_back(DisableWaiter(process, callback));
				spawn();
				return false;
			} else {
				assert(count - disablingCount > 1);
				processes.erase(process->it);
				disabledProcesses.push_back(process);
				pqueue.erase(process->pqHandle);
				process->it = disabledProcesses.last_iterator();
				process->enabled = Process::DISABLED;
				return true;
			}
		} else if (process->enabled == Process::DISABLING) {
			disableWaitlist.push_back(DisableWaiter(process, callback));
			return false;
		} else {
			return true;
		}
	}
	
	void asyncCleanupSpawner() {
		createInterruptableThread(
			boost::bind(&Spawner::cleanup, spawner),
			"Group spawner cleanup: " + name,
			POOL_HELPER_THREAD_STACK_SIZE);
	}

	unsigned int usage() const {
		int result = count;
		if (spawning()) {
			result++;
		}
		return result;
	}
	
	bool garbageCollectable(unsigned long long now = 0) const {
		if (now == 0) {
			now = SystemTime::getUsec();
		}
		return usage() == 0
			&& getWaitlist.empty()
			&& disabledProcesses.empty()
			&& options.getSpawnerTimeout() != 0
			&& now - spawner->lastUsed() >
				(unsigned long long) options.getSpawnerTimeout() * 1000000;
	}
	
	/** Whether a new process should be spawned for this group in case
	 * another get action is to be performed.
	 */
	bool shouldSpawn() const;
	
	/** Start spawning a new process in the background, in case this
	 * isn't already happening. Will ensure that at least options.minProcesses
	 * are started.
	 */
	void spawn() {
		if (!spawning()) {
			createInterruptableThread(
				boost::bind(&Group::spawnThreadMain,
					this, shared_from_this(), spawner,
					options.copyAndPersist().clearPerRequestFields()),
				"Group process spawner: " + name,
				POOL_HELPER_THREAD_STACK_SIZE);
			m_spawning = true;
		}
	}
	
	bool needsRestart(const Options &options) {
		struct stat buf;
		return cstat.stat(alwaysRestartFile, &buf, options.statThrottleRate) == 0 ||
		       fileChangeChecker.changed(restartFile, options.statThrottleRate);
	}
	
	void restart(const Options &options);
	
	bool spawning() const {
		return m_spawning;
	}
};


} // namespace ApplicationPool2
} // namespace Passenger

#endif /* _PASSENGER_APPLICATION_POOL2_GROUP_H_ */
