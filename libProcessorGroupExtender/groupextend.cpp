/*
* (c)2020 Jeremy Collake <jeremy@bitsum.com>, Bitsum LLC
*/
#include "pch.h"
#include "groupextend.h"
#include "framework.h"
#include "helpers.h"
#include "LogOut.h"
#include "../version.h"
#include "../entry.h"

// async wrapper implemented in header file

// the meat
int ProcessorGroupExtender_SingleProcess::ExtendGroupForProcess()
{
	m_log.Write(L"\n Monitoring process %u with refresh rate of %u ms", m_pid, m_nRefreshRateMs);
	
	_ASSERT(m_pid && m_nRefreshRateMs>=GroupExtend::REFRESH_MINIMUM_ALLOWED_MS);

	unsigned short nActiveGroupCount = static_cast<unsigned short>(GetActiveProcessorGroupCount());
	if (nActiveGroupCount < 2)
	{
		m_log.Write(L"\n ERROR: Active processor groups is only %u. Nothing to do, aborting.", nActiveGroupCount);		
		if (m_hThreadStoppedEvent) SetEvent(m_hThreadStoppedEvent);
		return 2;
	}

	// should be equal size groups, but check each group to support theoretical variance in size
	std::vector<unsigned int> vecProcessorsPerGroup;
	std::vector<unsigned long long> vecAllCPUMaskPerGroup;
	for (unsigned short curGroup = 0; curGroup < nActiveGroupCount; curGroup++)
	{
		// store the processor count for this group
		vecProcessorsPerGroup.push_back(GetActiveProcessorCount(curGroup));
		// and the all CPU affinity mask for this group
		vecAllCPUMaskPerGroup.push_back(BuildAffinityMask(vecProcessorsPerGroup[curGroup]));
	}

	// now get group info for target process
	std::vector<unsigned short> vecGroupsThisProcess;
	if (!GetProcessProcessorGroups(m_pid, vecGroupsThisProcess))
	{
		m_log.Write(L"\n ERROR: GetProcessProcessorGroups returned 0. Aborting.");
		if (m_hThreadStoppedEvent) SetEvent(m_hThreadStoppedEvent);
		return 3;
	}
	if (vecGroupsThisProcess.size() > 1)
	{
		m_log.Write(L"\n WARNING: Process is already multi-group! This algorithm is not (yet) designed to handle this situation.");
		// continue on, debatably
	}
	m_log.Write(L"\n Process currently has threads on group(s)");
	for (auto& i : vecGroupsThisProcess)
	{
		m_log.Write(L" %u", i);
	}
	unsigned short nDefaultGroupId = vecGroupsThisProcess[0];

	// track all thread assignments to processor groups (all threads in the managed app)
	std::map<unsigned long, unsigned short> mapThreadIDsToProcessorGroupNum;

	// keep count of threads per group (group ID is index)
	std::vector<unsigned long> vecThreadCountPerGroup;

	// initialize vector
	for (unsigned short n = 0; n < nActiveGroupCount; n++)
	{
		vecThreadCountPerGroup.push_back(0);
	}

	do
	{
		HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (hSnapshot == INVALID_HANDLE_VALUE)
		{
			m_log.FormattedErrorOut(L" CreateToolhelp32Snapshot");
			return 3;
		}

		if (hSnapshot)
		{
			THREADENTRY32 te32;
			te32.dwSize = sizeof(THREADENTRY32);
			if (!Thread32First(hSnapshot, &te32))
			{
				m_log.FormattedErrorOut(L" Thread32First");
				CloseHandle(hSnapshot);
				return(FALSE);
			}

			// for marking each thread ID as found so we can identify deleted threads and remove from mapThreadIDsToProcessorGroupNum
			std::map<unsigned long, bool> mapThreadIDsFoundThisEnum;
			do
			{
				if (m_pid == te32.th32OwnerProcessID)
				{
					mapThreadIDsFoundThisEnum[te32.th32ThreadID] = true;
				}
			} while (Thread32Next(hSnapshot, &te32));

			// remove deleted threads
			std::vector<unsigned long> vecPendingThreadIDDeletions;
			for (auto& i : mapThreadIDsToProcessorGroupNum)
			{
				if (mapThreadIDsFoundThisEnum.find(i.first) == mapThreadIDsFoundThisEnum.end())
				{
					vecPendingThreadIDDeletions.push_back(i.first);
				}
			}
			for (auto& i : vecPendingThreadIDDeletions)
			{
				unsigned short nGroupId = mapThreadIDsToProcessorGroupNum.find(i)->second;
				vecThreadCountPerGroup[nGroupId]--;
				mapThreadIDsToProcessorGroupNum.erase(i);
				m_log.Write(L"\n Thread %u terminated on group %u", i, nGroupId);
			}
			// add new threads
			std::vector<unsigned long> vecPendingThreadIDAdditions;
			for (auto& i : mapThreadIDsFoundThisEnum)
			{
				if (mapThreadIDsToProcessorGroupNum.find(i.first) == mapThreadIDsToProcessorGroupNum.end())
				{
					vecPendingThreadIDAdditions.push_back(i.first);
				}
			}
			for (auto& i : vecPendingThreadIDAdditions)
			{
				unsigned short nGroupId = GroupExtend::INVALID_GROUP_ID;
				// determine target group (if room on default group, use it, then use others)
				// if not default group, check CPU assignment mask of group for first free CPU, and use it
				if (vecThreadCountPerGroup[nDefaultGroupId] < vecProcessorsPerGroup[nDefaultGroupId])
				{
					nGroupId = nDefaultGroupId;
					m_log.Write(L"\n Leaving thread in default group.");
				}
				else
				{
					for (int n = 0; n < nActiveGroupCount; n++)
					{
						// only check groups other than default
						if (n != nDefaultGroupId)
						{
							if (vecThreadCountPerGroup[n] < vecProcessorsPerGroup[n])
							{
								nGroupId = n;
								break;
							}
						}
					}
				}
				if (nGroupId == GroupExtend::INVALID_GROUP_ID)
				{
					nGroupId = nDefaultGroupId;
					m_log.Write(L"\n No space in supplemental group(s), leaving in default group");
				}
				// if not default group, then select specific CPU
				if (nGroupId != nDefaultGroupId)
				{
					HANDLE hThread = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, i);
					if (hThread)
					{
						GROUP_AFFINITY grpAffinity = {};
						grpAffinity.Group = nGroupId;
						grpAffinity.Mask = vecAllCPUMaskPerGroup[nGroupId];
						DWORD_PTR dwPriorAffinity = SetThreadGroupAffinity(hThread, &grpAffinity, nullptr);
						CloseHandle(hThread);
						if (!dwPriorAffinity)
						{
							// error, so leave in default group						
							nGroupId = nDefaultGroupId;
							m_log.Write(L"\n WARNING: Error setting thread affinity for %u (terminated too quick?). Leaving in default group.", i);
						}
					}
					else
					{
						// no access, so leave in default group						
						nGroupId = nDefaultGroupId;
						m_log.Write(L"\n WARNING: No access to thread %u. Leaving in default group.", i);
					}
				}
				m_log.Write(L"\n Thread %u found, group %u", i, nGroupId);
				vecThreadCountPerGroup[nGroupId]++;
				mapThreadIDsToProcessorGroupNum[i] = nGroupId;				
			}

			m_log.Write(L"\n Managing %u threads", mapThreadIDsToProcessorGroupNum.size());
			for (unsigned short n = 0; n < nActiveGroupCount; n++)
			{
				m_log.Write(L"\n Group %u has %u threads", n, vecThreadCountPerGroup[n]);
			}

			if (!mapThreadIDsToProcessorGroupNum.size())
			{
				m_log.Write(L"\n No threads to manage, exiting ...");
				break;
			}
			CloseHandle(hSnapshot);
		}
	} while (WaitForSingleObject(m_hQuitNotifyEvent, m_nRefreshRateMs) == WAIT_TIMEOUT);

	// signal caller that our thread stopped
	if (m_hThreadStoppedEvent) SetEvent(m_hThreadStoppedEvent);
	return 0;
}