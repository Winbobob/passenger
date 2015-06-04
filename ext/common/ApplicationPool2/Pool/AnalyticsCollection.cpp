/*
 *  Phusion Passenger - https://www.phusionpassenger.com/
 *  Copyright (c) 2011-2015 Phusion
 *
 *  "Phusion Passenger" is a trademark of Hongli Lai & Ninh Bui.
 *
 *  See LICENSE file for license information.
 */
#include <ApplicationPool2/Pool.h>

/*************************************************************************
 *
 * Analytics collection functions for ApplicationPool2::Pool
 *
 *************************************************************************/

namespace Passenger {
namespace ApplicationPool2 {

using namespace std;
using namespace boost;


void
Pool::initializeAnalyticsCollection() {
	interruptableThreads.create_thread(
		boost::bind(collectAnalytics, shared_from_this()),
		"Pool analytics collector",
		POOL_HELPER_THREAD_STACK_SIZE
	);
}

void
Pool::collectAnalytics(PoolPtr self) {
	TRACE_POINT();
	syscalls::usleep(3000000);
	while (!this_thread::interruption_requested()) {
		try {
			UPDATE_TRACE_POINT();
			self->realCollectAnalytics();
		} catch (const thread_interrupted &) {
			break;
		} catch (const tracable_exception &e) {
			P_WARN("ERROR: " << e.what() << "\n  Backtrace:\n" << e.backtrace());
		}

		// Sleep for about 4 seconds, aligned to seconds boundary
		// for saving power on laptops.
		UPDATE_TRACE_POINT();
		unsigned long long currentTime = SystemTime::getUsec();
		unsigned long long deadline =
			roundUp<unsigned long long>(currentTime, 1000000) + 4000000;
		P_DEBUG("Analytics collection done; next analytics collection in " <<
			std::fixed << std::setprecision(3) << ((deadline - currentTime) / 1000000.0) <<
			" sec");
		try {
			syscalls::usleep(deadline - currentTime);
		} catch (const thread_interrupted &) {
			break;
		} catch (const tracable_exception &e) {
			P_WARN("ERROR: " << e.what() << "\n  Backtrace:\n" << e.backtrace());
		}
	}
}

void
Pool::collectPids(const ProcessList &processes, vector<pid_t> &pids) {
	foreach (const ProcessPtr &process, processes) {
		pids.push_back(process->getPid());
	}
}

void
Pool::updateProcessMetrics(const ProcessList &processes,
	const ProcessMetricMap &allMetrics,
	vector<ProcessPtr> &processesToDetach)
{
	foreach (const ProcessPtr &process, processes) {
		ProcessMetricMap::const_iterator metrics_it =
			allMetrics.find(process->getPid());
		if (metrics_it != allMetrics.end()) {
			process->metrics = metrics_it->second;

			// Check memory limit.
			Group *group = process->getGroup();
			if (group->options.memoryLimit > 0
			 && process->metrics.realMemory() / 1024 > group->options.memoryLimit)
			{
				P_WARN("*** Process " << process->inspect() << " is now using " <<
					process->metrics.realMemory() / 1024 <<  " MB of memory, "
					"which exceeds its limit of " << group->options.memoryLimit <<
					" MB. Shutting it down and detaching it...");
				processesToDetach.push_back(process);
			}

		// If the process is missing from 'allMetrics' then either 'ps'
		// failed or the process really is gone. We double check by sending
		// it a signal.
		} else if (!process->isDummy() && !process->osProcessExists()) {
			P_WARN("Process " << process->inspect() << " no longer exists! "
				"Detaching it from the pool.");
			processesToDetach.push_back(process);
		}
	}
}

void
Pool::prepareUnionStationProcessStateLogs(vector<UnionStationLogEntry> &logEntries,
	const GroupPtr &group) const
{
	const UnionStation::CorePtr &unionStationCore = getUnionStationCore();
	if (group->options.analytics && unionStationCore != NULL) {
		logEntries.push_back(UnionStationLogEntry());
		UnionStationLogEntry &entry = logEntries.back();
		stringstream stream;

		stream << "Group: <group>";
		group->inspectXml(stream, false);
		stream << "</group>";

		entry.groupName = group->options.getAppGroupName();
		entry.category  = "processes";
		entry.key       = group->options.unionStationKey;
		entry.data      = stream.str();
	}
}

void
Pool::prepareUnionStationSystemMetricsLogs(vector<UnionStationLogEntry> &logEntries,
	const GroupPtr &group) const
{
	const UnionStation::CorePtr &unionStationCore = getUnionStationCore();
	if (group->options.analytics && unionStationCore != NULL) {
		logEntries.push_back(UnionStationLogEntry());
		UnionStationLogEntry &entry = logEntries.back();
		stringstream stream;

		stream << "System metrics: ";
		systemMetrics.toXml(stream);

		entry.groupName = group->options.getAppGroupName();
		entry.category  = "system_metrics";
		entry.key       = group->options.unionStationKey;
		entry.data      = stream.str();
	}
}

void
Pool::realCollectAnalytics() {
	TRACE_POINT();
	this_thread::disable_interruption di;
	this_thread::disable_syscall_interruption dsi;
	vector<pid_t> pids;
	unsigned int max;

	P_DEBUG("Analytics collection time...");
	// Collect all the PIDs.
	{
		UPDATE_TRACE_POINT();
		LockGuard l(syncher);
		max = this->max;
	}
	pids.reserve(max);
	{
		UPDATE_TRACE_POINT();
		LockGuard l(syncher);
		GroupMap::ConstIterator g_it(groups);

		while (*g_it != NULL) {
			const GroupPtr &group = g_it.getValue();
			collectPids(group->enabledProcesses, pids);
			collectPids(group->disablingProcesses, pids);
			collectPids(group->disabledProcesses, pids);
			g_it.next();
		}
	}

	// Collect process metrics and system and store them in the
	// data structures. Later, we log them to Union Station.
	ProcessMetricMap processMetrics;
	try {
		UPDATE_TRACE_POINT();
		P_DEBUG("Collecting process metrics");
		processMetrics = ProcessMetricsCollector().collect(pids);
	} catch (const ParseException &) {
		P_WARN("Unable to collect process metrics: cannot parse 'ps' output.");
		return;
	}
	try {
		UPDATE_TRACE_POINT();
		P_DEBUG("Collecting system metrics");
		systemMetricsCollector.collect(systemMetrics);
	} catch (const RuntimeException &e) {
		P_WARN("Unable to collect system metrics: " << e.what());
		return;
	}

	{
		UPDATE_TRACE_POINT();
		vector<UnionStationLogEntry> logEntries;
		vector<ProcessPtr> processesToDetach;
		boost::container::vector<Callback> actions;
		ScopedLock l(syncher);
		GroupMap::ConstIterator g_it(groups);

		UPDATE_TRACE_POINT();
		while (*g_it != NULL) {
			const GroupPtr &group = g_it.getValue();
			updateProcessMetrics(group->enabledProcesses, processMetrics, processesToDetach);
			updateProcessMetrics(group->disablingProcesses, processMetrics, processesToDetach);
			updateProcessMetrics(group->disabledProcesses, processMetrics, processesToDetach);
			prepareUnionStationProcessStateLogs(logEntries, group);
			prepareUnionStationSystemMetricsLogs(logEntries, group);
			g_it.next();
		}

		UPDATE_TRACE_POINT();
		foreach (const ProcessPtr process, processesToDetach) {
			detachProcessUnlocked(process, actions);
		}
		UPDATE_TRACE_POINT();
		processesToDetach.clear();

		l.unlock();
		UPDATE_TRACE_POINT();
		if (!logEntries.empty()) {
			const UnionStation::CorePtr &unionStationCore = getUnionStationCore();
			P_DEBUG("Sending process and system metrics to Union Station");
			while (!logEntries.empty()) {
				UnionStationLogEntry &entry = logEntries.back();
				UnionStation::TransactionPtr transaction =
					unionStationCore->newTransaction(
						entry.groupName,
						entry.category,
						entry.key);
				transaction->message(entry.data);
				logEntries.pop_back();
			}
		}

		UPDATE_TRACE_POINT();
		runAllActions(actions);
		UPDATE_TRACE_POINT();
		// Run destructors with updated trace point.
		actions.clear();
	}
}


} // namespace ApplicationPool2
} // namespace Passenger
