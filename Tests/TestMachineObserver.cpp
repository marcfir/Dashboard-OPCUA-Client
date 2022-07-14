/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Copyright 2020 (c) Dominik Basner, Sotec GmbH (for VDW e.V.)
 */

#include <gtest/gtest.h>
#include <opcua_proxystub.h>
#include <uadiscovery.h>
#include <uaplatformlayer.h>
#include <uasession.h>

#include <ConfigureLogger.hpp>
#include <MachineObserver.hpp>
#include <OpcUaClient.hpp>
#include <algorithm>

#include "../../_install-Debug/include/gmock/gmock-spec-builders.h"
#include "gmock/gmock.h"
#include "uaclientsdk_mock.hpp"

#define OPCUA_TEST_SERVER_URL "opc.tcp://localhost:48010"

using ::testing::AtLeast;
using ::testing::DoAll;
using ::testing::Expectation;
using ::testing::Invoke;
using ::testing::Return;
using ::testing::SetArgReferee;

namespace Umati {
namespace MachineObserver {
class TestMachineObserver : public MachineObserver {};
}  // namespace MachineObserver
}  // namespace Umati
