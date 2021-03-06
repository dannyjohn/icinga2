/******************************************************************************
 * Icinga 2                                                                   *
 * Copyright (C) 2012-2015 Icinga Development Team (http://www.icinga.org)    *
 *                                                                            *
 * This program is free software; you can redistribute it and/or              *
 * modify it under the terms of the GNU General Public License                *
 * as published by the Free Software Foundation; either version 2             *
 * of the License, or (at your option) any later version.                     *
 *                                                                            *
 * This program is distributed in the hope that it will be useful,            *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              *
 * GNU General Public License for more details.                               *
 *                                                                            *
 * You should have received a copy of the GNU General Public License          *
 * along with this program; if not, write to the Free Software Foundation     *
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.             *
 ******************************************************************************/

#include "checker/checkercomponent.hpp"
#include "icinga/icingaapplication.hpp"
#include "icinga/cib.hpp"
#include "icinga/perfdatavalue.hpp"
#include "remote/apilistener.hpp"
#include "base/dynamictype.hpp"
#include "base/objectlock.hpp"
#include "base/utility.hpp"
#include "base/logger.hpp"
#include "base/exception.hpp"
#include "base/convert.hpp"
#include "base/statsfunction.hpp"
#include <boost/foreach.hpp>

using namespace icinga;

REGISTER_TYPE(CheckerComponent);

REGISTER_STATSFUNCTION(CheckerComponentStats, &CheckerComponent::StatsFunc);

void CheckerComponent::StatsFunc(const Dictionary::Ptr& status, const Array::Ptr& perfdata)
{
	Dictionary::Ptr nodes = new Dictionary();

	BOOST_FOREACH(const CheckerComponent::Ptr& checker, DynamicType::GetObjectsByType<CheckerComponent>()) {
		unsigned long idle = checker->GetIdleCheckables();
		unsigned long pending = checker->GetPendingCheckables();

		Dictionary::Ptr stats = new Dictionary();
		stats->Set("idle", idle);
		stats->Set("pending", pending);

		nodes->Set(checker->GetName(), stats);

		String perfdata_prefix = "checkercomponent_" + checker->GetName() + "_";
		perfdata->Add(new PerfdataValue(perfdata_prefix + "idle", Convert::ToDouble(idle)));
		perfdata->Add(new PerfdataValue(perfdata_prefix + "pending", Convert::ToDouble(pending)));
	}

	status->Set("checkercomponent", nodes);
}

void CheckerComponent::OnConfigLoaded(void)
{
	DynamicObject::OnStarted.connect(bind(&CheckerComponent::ObjectHandler, this, _1));
	DynamicObject::OnStopped.connect(bind(&CheckerComponent::ObjectHandler, this, _1));
	DynamicObject::OnPaused.connect(bind(&CheckerComponent::ObjectHandler, this, _1));
	DynamicObject::OnResumed.connect(bind(&CheckerComponent::ObjectHandler, this, _1));

	Checkable::OnNextCheckChanged.connect(bind(&CheckerComponent::NextCheckChangedHandler, this, _1));
}

void CheckerComponent::Start(void)
{
	DynamicObject::Start();

	m_Stopped = false;

	m_Thread = boost::thread(boost::bind(&CheckerComponent::CheckThreadProc, this));

	m_ResultTimer = new Timer();
	m_ResultTimer->SetInterval(5);
	m_ResultTimer->OnTimerExpired.connect(boost::bind(&CheckerComponent::ResultTimerHandler, this));
	m_ResultTimer->Start();
}

void CheckerComponent::Stop(void)
{
	Log(LogInformation, "CheckerComponent", "Checker stopped.");

	{
		boost::mutex::scoped_lock lock(m_Mutex);
		m_Stopped = true;
		m_CV.notify_all();
	}

	m_ResultTimer->Stop();
	m_Thread.join();

	DynamicObject::Stop();
}

void CheckerComponent::CheckThreadProc(void)
{
	Utility::SetThreadName("Check Scheduler");

	boost::mutex::scoped_lock lock(m_Mutex);

	for (;;) {
		typedef boost::multi_index::nth_index<CheckableSet, 1>::type CheckTimeView;
		CheckTimeView& idx = boost::get<1>(m_IdleCheckables);

		while (idx.begin() == idx.end() && !m_Stopped)
			m_CV.wait(lock);

		if (m_Stopped)
			break;

		CheckTimeView::iterator it = idx.begin();
		Checkable::Ptr checkable = *it;

		double wait = checkable->GetNextCheck() - Utility::GetTime();

		if (wait > 0) {
			/* Wait for the next check. */
			m_CV.timed_wait(lock, boost::posix_time::milliseconds(wait * 1000));

			continue;
		}

		m_IdleCheckables.erase(checkable);

		bool forced = checkable->GetForceNextCheck();
		bool check = true;

		if (!forced) {
			if (!checkable->IsReachable(DependencyCheckExecution)) {
				Log(LogNotice, "CheckerComponent")
				    << "Skipping check for object '" << checkable->GetName() << "': Dependency failed.";
				check = false;
			}

			Host::Ptr host;
			Service::Ptr service;
			tie(host, service) = GetHostService(checkable);

			if (host && !service && (!checkable->GetEnableActiveChecks() || !IcingaApplication::GetInstance()->GetEnableHostChecks())) {
				Log(LogNotice, "CheckerComponent")
				    << "Skipping check for host '" << host->GetName() << "': active host checks are disabled";
				check = false;
			}
			if (host && service && (!checkable->GetEnableActiveChecks() || !IcingaApplication::GetInstance()->GetEnableServiceChecks())) {
				Log(LogNotice, "CheckerComponent")
				    << "Skipping check for service '" << service->GetName() << "': active service checks are disabled";
				check = false;
			}

			TimePeriod::Ptr tp = checkable->GetCheckPeriod();

			if (tp && !tp->IsInside(Utility::GetTime())) {
				Log(LogNotice, "CheckerComponent")
				    << "Skipping check for object '" << checkable->GetName() << "': not in check_period";
				check = false;
			}
		}

		/* reschedule the checkable if checks are disabled */
		if (!check) {
			m_IdleCheckables.insert(checkable);
			lock.unlock();

			checkable->UpdateNextCheck();

			lock.lock();

			continue;
		}

		m_PendingCheckables.insert(checkable);

		lock.unlock();

		if (forced) {
			ObjectLock olock(checkable);
			checkable->SetForceNextCheck(false);
		}

		Log(LogDebug, "CheckerComponent")
		    << "Executing check for '" << checkable->GetName() << "'";

		Utility::QueueAsyncCallback(boost::bind(&CheckerComponent::ExecuteCheckHelper, CheckerComponent::Ptr(this), checkable));

		lock.lock();
	}
}

void CheckerComponent::ExecuteCheckHelper(const Checkable::Ptr& checkable)
{
	try {
		checkable->ExecuteCheck();
	} catch (const std::exception& ex) {
		CheckResult::Ptr cr = new CheckResult();
		cr->SetState(ServiceUnknown);

		String output = "Exception occured while checking '" + checkable->GetName() + "': " + DiagnosticInformation(ex);
		cr->SetOutput(output);

		double now = Utility::GetTime();
		cr->SetScheduleStart(now);
		cr->SetScheduleEnd(now);
		cr->SetExecutionStart(now);
		cr->SetExecutionEnd(now);

		checkable->ProcessCheckResult(cr);

		Log(LogCritical, "checker", output);
	}

	{
		boost::mutex::scoped_lock lock(m_Mutex);

		/* remove the object from the list of pending objects; if it's not in the
		 * list this was a manual (i.e. forced) check and we must not re-add the
		 * object to the list because it's already there. */
		CheckerComponent::CheckableSet::iterator it;
		it = m_PendingCheckables.find(checkable);
		if (it != m_PendingCheckables.end()) {
			m_PendingCheckables.erase(it);

			if (checkable->IsActive())
				m_IdleCheckables.insert(checkable);

			m_CV.notify_all();
		}
	}

	Log(LogDebug, "CheckerComponent")
	    << "Check finished for object '" << checkable->GetName() << "'";
}

void CheckerComponent::ResultTimerHandler(void)
{
	std::ostringstream msgbuf;

	{
		boost::mutex::scoped_lock lock(m_Mutex);

		msgbuf << "Pending checkables: " << m_PendingCheckables.size() << "; Idle checkables: " << m_IdleCheckables.size() << "; Checks/s: "
		    << (CIB::GetActiveHostChecksStatistics(5) + CIB::GetActiveServiceChecksStatistics(5)) / 5.0;
	}

	Log(LogNotice, "CheckerComponent", msgbuf.str());
}

void CheckerComponent::ObjectHandler(const DynamicObject::Ptr& object)
{
	if (!Type::GetByName("Checkable")->IsAssignableFrom(object->GetReflectionType()))
		return;

	Checkable::Ptr checkable = static_pointer_cast<Checkable>(object);

	Zone::Ptr zone = Zone::GetByName(checkable->GetZoneName());
	bool same_zone = (!zone || Zone::GetLocalZone() == zone);

	{
		boost::mutex::scoped_lock lock(m_Mutex);

		if (object->IsActive() && !object->IsPaused() && same_zone) {
			if (m_PendingCheckables.find(checkable) != m_PendingCheckables.end())
				return;

			m_IdleCheckables.insert(checkable);
		} else {
			m_IdleCheckables.erase(checkable);
			m_PendingCheckables.erase(checkable);
		}

		m_CV.notify_all();
	}
}

void CheckerComponent::NextCheckChangedHandler(const Checkable::Ptr& checkable)
{
	boost::mutex::scoped_lock lock(m_Mutex);

	/* remove and re-insert the object from the set in order to force an index update */
	typedef boost::multi_index::nth_index<CheckableSet, 0>::type CheckableView;
	CheckableView& idx = boost::get<0>(m_IdleCheckables);

	CheckableView::iterator it = idx.find(checkable);
	if (it == idx.end())
		return;

	idx.erase(checkable);
	idx.insert(checkable);
	m_CV.notify_all();
}

unsigned long CheckerComponent::GetIdleCheckables(void)
{
	boost::mutex::scoped_lock lock(m_Mutex);

	return m_IdleCheckables.size();
}

unsigned long CheckerComponent::GetPendingCheckables(void)
{
	boost::mutex::scoped_lock lock(m_Mutex);

	return m_PendingCheckables.size();
}
