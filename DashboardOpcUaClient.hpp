/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Copyright 2021 (c) Christian von Arnim, ISW University of Stuttgart (for umati and VDW e.V.)
 */

#pragma once

#include <DashboardClient.hpp>
#include <DashboardMachineObserver.hpp>
#include <MqttPublisher_Paho.hpp>
#include <OpcUaTypeReader.hpp>
#include <functional>
#include <memory>

#include "MachineObserver/Topics.hpp"
#include "OpcUaClient/OpcUaClient.hpp"
#include "Util/Configuration.hpp"

class DashboardOpcUaClient {
 public:
  DashboardOpcUaClient(std::shared_ptr<Umati::Util::Configuration> configuration, std::function<void()> issueReset);

  bool connect(std::atomic_bool &running);
  void ReadTypes();
  void StartMachineObserver();
  void Iterate();

 protected:
  std::function<void()> m_issueReset;
  std::shared_ptr<Umati::OpcUa::OpcUaInterface> m_opcUaWrapper;
  std::shared_ptr<Umati::OpcUa::OpcUaClient> m_pClient;
  std::shared_ptr<Umati::MqttPublisher_Paho::MqttPublisher_Paho> m_pPublisher;
  std::shared_ptr<Umati::Dashboard::OpcUaTypeReader> m_pOpcUaTypeReader;
  std::shared_ptr<Umati::MachineObserver::DashboardMachineObserver> m_pMachineObserver;
  std::chrono::time_point<std::chrono::steady_clock> m_lastPublish;
  std::chrono::time_point<std::chrono::steady_clock> m_lastConnectionVerify;
};
