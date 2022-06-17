#include "button_handler.hpp"

#include "settings.hpp"

#include <fmt/format.h>

#include <iomanip>
#include <phosphor-logging/log.hpp>
#include <sstream>
#include <xyz/openbmc_project/State/Chassis/server.hpp>
#include <xyz/openbmc_project/State/Host/server.hpp>

namespace phosphor
{
namespace button
{

namespace sdbusRule = sdbusplus::bus::match::rules;
using namespace sdbusplus::xyz::openbmc_project::State::server;
using namespace phosphor::logging;

constexpr auto propertyIface = "org.freedesktop.DBus.Properties";
constexpr auto chassisIface = "xyz.openbmc_project.State.Chassis";
constexpr auto hostIface = "xyz.openbmc_project.State.Host";
constexpr auto powerButtonIface = "xyz.openbmc_project.Chassis.Buttons.Power";
constexpr auto idButtonIface = "xyz.openbmc_project.Chassis.Buttons.ID";
constexpr auto resetButtonIface = "xyz.openbmc_project.Chassis.Buttons.Reset";
constexpr auto mapperIface = "xyz.openbmc_project.ObjectMapper";
constexpr auto ledGroupIface = "xyz.openbmc_project.Led.Group";

constexpr auto mapperObjPath = "/xyz/openbmc_project/object_mapper";
constexpr auto mapperService = "xyz.openbmc_project.ObjectMapper";
constexpr auto ledGroupBasePath = "/xyz/openbmc_project/led/groups/";

Handler::Handler(sdbusplus::bus::bus& bus) : bus(bus)
{
    try
    {
        if (!getService(POWER_DBUS_OBJECT_NAME, powerButtonIface).empty())
        {
            log<level::INFO>("Starting power button handler");
            powerButtonPressed = std::make_unique<sdbusplus::bus::match_t>(
                bus,
                sdbusRule::type::signal() + sdbusRule::member("Pressed") +
                    sdbusRule::path(POWER_DBUS_OBJECT_NAME) +
                    sdbusRule::interface(powerButtonIface),
                std::bind(std::mem_fn(&Handler::powerBtnPressed), this,
                          std::placeholders::_1));

            powerButtonReleased = std::make_unique<sdbusplus::bus::match_t>(
                bus,
                sdbusRule::type::signal() + sdbusRule::member("Released") +
                    sdbusRule::path(POWER_DBUS_OBJECT_NAME) +
                    sdbusRule::interface(powerButtonIface),
                std::bind(std::mem_fn(&Handler::powerBtnReleased), this,
                          std::placeholders::_1));

            powerButtonLongPressReleased =
                std::make_unique<sdbusplus::bus::match_t>(
                    bus,
                    sdbusRule::type::signal() +
                        sdbusRule::member("PressedLong") +
                        sdbusRule::path(POWER_DBUS_OBJECT_NAME) +
                        sdbusRule::interface(powerButtonIface),
                    std::bind(std::mem_fn(&Handler::longPowerPressed), this,
                              std::placeholders::_1));

            powerOpTimer = std::make_unique<
                sdeventplus::utility::Timer<sdeventplus::ClockId::Monotonic>>(
                bus.get_event(), std::bind(&Handler::timerHandler, this),
                pollInterval);
            powerOpTimer->setEnabled(false);
            powerOpState = PowerOpState::buttonNotPressed;
        }
    }
    catch (const sdbusplus::exception::exception& e)
    {
        // The button wasn't implemented
    }

    try
    {
        if (!getService(ID_DBUS_OBJECT_NAME, idButtonIface).empty())
        {
            log<level::INFO>("Registering ID button handler");
            idButtonReleased = std::make_unique<sdbusplus::bus::match_t>(
                bus,
                sdbusRule::type::signal() + sdbusRule::member("Released") +
                    sdbusRule::path(ID_DBUS_OBJECT_NAME) +
                    sdbusRule::interface(idButtonIface),
                std::bind(std::mem_fn(&Handler::idPressed), this,
                          std::placeholders::_1));
        }
    }
    catch (const sdbusplus::exception::exception& e)
    {
        // The button wasn't implemented
    }

    try
    {
        if (!getService(RESET_DBUS_OBJECT_NAME, resetButtonIface).empty())
        {
            log<level::INFO>("Registering reset button handler");
            resetButtonReleased = std::make_unique<sdbusplus::bus::match_t>(
                bus,
                sdbusRule::type::signal() + sdbusRule::member("Released") +
                    sdbusRule::path(RESET_DBUS_OBJECT_NAME) +
                    sdbusRule::interface(resetButtonIface),
                std::bind(std::mem_fn(&Handler::resetPressed), this,
                          std::placeholders::_1));
        }
    }
    catch (const sdbusplus::exception::exception& e)
    {
        // The button wasn't implemented
    }
}

std::string Handler::getService(const std::string& path,
                                const std::string& interface) const
{
    auto method = bus.new_method_call(mapperService, mapperObjPath, mapperIface,
                                      "GetObject");
    method.append(path, std::vector{interface});
    auto result = bus.call(method);

    std::map<std::string, std::vector<std::string>> objectData;
    result.read(objectData);

    return objectData.begin()->first;
}

bool Handler::isBmcReady() const
{
    constexpr auto bmcObjectPath = "/xyz/openbmc_project/state/bmc0";
    constexpr auto bmcInterface = "xyz.openbmc_project.State.BMC";
    std::string bmcState;
    try
    {
        auto service = getService(bmcObjectPath, bmcInterface);
        auto method = bus.new_method_call(service.c_str(), bmcObjectPath,
                                          propertyIface, "Get");
        method.append(bmcInterface, "CurrentBMCState");
        auto result = bus.call(method);

        std::variant<std::string> val;
        result.read(val);

        if (auto pVal = std::get_if<std::string>(&val))
        {
            bmcState = *pVal;
        }
    }
    catch (const sdbusplus::exception::exception& e)
    {
        log<level::ERR>(
            fmt::format("{}: Exception - {}", std::string(__func__), e.what())
                .c_str());
    }

    return bmcState == "xyz.openbmc_project.State.BMC.BMCState.Ready";
}

bool Handler::poweredOn() const
{
    auto service = getService(CHASSIS_STATE_OBJECT_NAME, chassisIface);
    auto method = bus.new_method_call(
        service.c_str(), CHASSIS_STATE_OBJECT_NAME, propertyIface, "Get");
    method.append(chassisIface, "CurrentPowerState");
    auto result = bus.call(method);

    std::variant<std::string> state;
    result.read(state);

    return Chassis::PowerState::On ==
           Chassis::convertPowerStateFromString(std::get<std::string>(state));
}

void Handler::powerPressed(sdbusplus::message::message& msg)
{
    auto transition = Host::Transition::On;

    try
    {
        if (poweredOn())
        {
            transition = Host::Transition::Off;
        }

        log<level::INFO>("Handling power button press");

        std::variant<std::string> state = convertForMessage(transition);

        auto service = getService(HOST_STATE_OBJECT_NAME, hostIface);
        auto method = bus.new_method_call(
            service.c_str(), HOST_STATE_OBJECT_NAME, propertyIface, "Set");
        method.append(hostIface, "RequestedHostTransition", state);

        bus.call(method);
    }
    catch (const sdbusplus::exception::exception& e)
    {
        log<level::ERR>("Failed power state change on a power button press",
                        entry("ERROR=%s", e.what()));
    }
}

void Handler::longPowerPressed(sdbusplus::message::message& msg)
{
    try
    {
        if (!poweredOn())
        {
            log<level::INFO>(
                "Power is off so ignoring long power button press");
            return;
        }

        log<level::INFO>("Handling long power button press");

        std::variant<std::string> state =
            convertForMessage(Chassis::Transition::Off);

        auto service = getService(CHASSIS_STATE_OBJECT_NAME, chassisIface);
        auto method = bus.new_method_call(
            service.c_str(), CHASSIS_STATE_OBJECT_NAME, propertyIface, "Set");
        method.append(chassisIface, "RequestedPowerTransition", state);

        bus.call(method);
    }
    catch (const sdbusplus::exception::exception& e)
    {
        log<level::ERR>("Failed powering off on long power button press",
                        entry("ERROR=%s", e.what()));
    }
}

void Handler::resetPressed(sdbusplus::message::message& msg)
{
    try
    {
        if (!poweredOn())
        {
            log<level::INFO>("Power is off so ignoring reset button press");
            return;
        }

        log<level::INFO>("Handling reset button press");

        std::variant<std::string> state =
            convertForMessage(Host::Transition::Reboot);

        auto service = getService(HOST_STATE_OBJECT_NAME, hostIface);
        auto method = bus.new_method_call(
            service.c_str(), HOST_STATE_OBJECT_NAME, propertyIface, "Set");

        method.append(hostIface, "RequestedHostTransition", state);

        bus.call(method);
    }
    catch (const sdbusplus::exception::exception& e)
    {
        log<level::ERR>("Failed power state change on a reset button press",
                        entry("ERROR=%s", e.what()));
    }
}

void Handler::idPressed(sdbusplus::message::message& msg)
{
    std::string groupPath{ledGroupBasePath};
    groupPath += ID_LED_GROUP;

    auto service = getService(groupPath, ledGroupIface);

    if (service.empty())
    {
        log<level::INFO>("No identify LED group found during ID button press",
                         entry("GROUP=%s", groupPath.c_str()));
        return;
    }

    try
    {
        auto method = bus.new_method_call(service.c_str(), groupPath.c_str(),
                                          propertyIface, "Get");
        method.append(ledGroupIface, "Asserted");
        auto result = bus.call(method);

        std::variant<bool> state;
        result.read(state);

        state = !std::get<bool>(state);

        log<level::INFO>("Changing ID LED group state on ID LED press",
                         entry("GROUP=%s", groupPath.c_str()),
                         entry("STATE=%d", std::get<bool>(state)));

        method = bus.new_method_call(service.c_str(), groupPath.c_str(),
                                     propertyIface, "Set");

        method.append(ledGroupIface, "Asserted", state);
        result = bus.call(method);
    }
    catch (const sdbusplus::exception::exception& e)
    {
        log<level::ERR>("Error toggling ID LED group on ID button press",
                        entry("ERROR=%s", e.what()));
    }
}

auto Handler::getPressTime() const
{
    return pressedTime;
}

void Handler::updatePressedTime(std::chrono::milliseconds timeout)
{
    pressedTime = std::chrono::steady_clock::now() + timeout;
}

void Handler::powerBtnPressed(sdbusplus::message::message& msg)
{
    try
    {
        if (!poweredOn())
        {
            if (!isBmcReady())
            {
                log<level::ERR>(
                    fmt::format("{}: BMC is not yet ready, try later",
                                std::string(__func__))
                        .c_str());
                return;
            }

            powerPressed(msg);
            return;
        }
    }
    catch (const sdbusplus::exception::exception& e)
    {
        log<level::ERR>(
            fmt::format("{}: Exception - {}", std::string(__func__), e.what())
                .c_str());
        return;
    }

    std::stringstream ss;

    // System is On, initiate the power Off process
    if (powerOpState == PowerOpState::buttonNotPressed)
    {
        powerOpState = PowerOpState::buttonPressed;
        updatePressedTime(defaultHoldDownInterval);

        // TODO: International symbol and countdown time to Op-Panel
        ss.clear();
        ss << std::right << std::setw(8) << "0?" << std::setw(8)
           << std::chrono::duration_cast<std::chrono::seconds>(
                  defaultHoldDownInterval)
                  .count();
        log<level::DEBUG>(fmt::format("{}", ss.str()).c_str());
        powerOpTimer->restart(pollInterval);
        return;
    }

    // Button press during DPO-FPO separation interval
    if (powerOpState == PowerOpState::dpoFpoSeparation)
    {
        powerOpState = PowerOpState::fpoInitiated;
        powerOpTimer->setEnabled(false);
        updatePressedTime(fpoInterval);

        // TODO: FPO SRC code and countdown to Op-Panel
        ss.clear();
        ss << std::right << std::setw(8) << "FPO SRC" << std::setw(8)
           << std::chrono::duration_cast<std::chrono::seconds>(fpoInterval)
                  .count();
        log<level::DEBUG>(fmt::format("{}", ss.str()).c_str());
        powerOpTimer->restart(pollInterval);
        return;
    }

    // Any other state, do nothing...
}

void Handler::powerBtnReleased(sdbusplus::message::message& msg)
{
    // Button released during default hold interval, do nothing...
    if (powerOpState == PowerOpState::buttonPressed)
    {
        powerOpState = PowerOpState::buttonNotPressed;
        powerOpTimer->setEnabled(false);
        return;
    }

    // Button released during DPO-FPO separation interval
    if (powerOpState == PowerOpState::dpoInitiated)
    {
        // prepare for normal shutdown after DPO-FPO separation time
        powerOpState = PowerOpState::dpoFpoSeparation;
        return;
    }

    // Button was pressed or pressed/released while in DPO-FPO separation
    // interval. FPO interval countdown
    if (powerOpState == PowerOpState::fpoInitiated)
    {
        return;
    }

    // Button released after FPO timeout expired
    powerOpState = PowerOpState::buttonNotPressed;
    powerOpTimer->setEnabled(false);
}

void Handler::timerHandler()
{
    // Timer
    std::stringstream ss;
    const auto now = std::chrono::steady_clock::now();

    // Default hold time countdown
    if (powerOpState == PowerOpState::buttonPressed)
    {
        if (now > getPressTime())
        {
            // Default hold time expired, enters DPO interval
            powerOpState = PowerOpState::dpoInitiated;
            updatePressedTime(dpoInterval);

            // TODO: DPO progress SRC code and countdown to Op-Panel
            ss.clear();
            ss << std::right << std::setw(8) << "DPO SRC" << std::setw(8)
               << std::chrono::duration_cast<std::chrono::seconds>(dpoInterval)
                      .count();
            log<level::DEBUG>(fmt::format("{}", ss.str()).c_str());
            return;
        }

        // TODO: International symbol and countdown time to Op-Panel
        ss.clear();
        ss << std::right << std::setw(8) << "0?" << std::setw(8)
           << std::chrono::duration_cast<std::chrono::seconds>(getPressTime() -
                                                               now)
                  .count();
        log<level::DEBUG>(fmt::format("{}", ss.str()).c_str());
    }

    // DPO-FPO separation interval countdown
    if (powerOpState == PowerOpState::dpoInitiated ||
        powerOpState == PowerOpState::dpoFpoSeparation)
    {
        if (now > getPressTime())
        {
            if (powerOpState == PowerOpState::dpoFpoSeparation)
            {
                // Button was released during DPO-FPO separation, power Off host
                powerOpState = PowerOpState::buttonNotPressed;
                sdbusplus::message::message msg; // dummy
                powerPressed(msg);
                powerOpTimer->setEnabled(false);
            }
            else
            {
                // Button still held down, DPO count expired, enter FPO interval
                powerOpState = PowerOpState::fpoInitiated;
                updatePressedTime(fpoInterval);

                // TODO: FPO SRC code and countdown to Op-Panel
                ss.clear();
                ss << std::right << std::setw(8) << "FPO SRC" << std::setw(8)
                   << std::chrono::duration_cast<std::chrono::seconds>(
                          fpoInterval)
                          .count();
                log<level::DEBUG>(fmt::format("{}", ss.str()).c_str());
            }
            return;
        }

        auto countDown = std::chrono::duration_cast<std::chrono::seconds>(
                             getPressTime() - now)
                             .count();

        // TODO: DPO SRC progress code and countdown to Op-Panel
        ss.clear();
        ss << std::right << std::setw(8) << "DPO SRC" << std::setw(8)
           << countDown;
        log<level::DEBUG>(fmt::format("{}", ss.str()).c_str());
    }

    // FPO interval countdown
    if (powerOpState == PowerOpState::fpoInitiated)
    {

        if (now > getPressTime())
        {
            // FPO count expired, power Off Chassis
            // TODO: SRC code before chassisoff to Op-Panel
            ss.clear();
            ss << std::right << std::setw(8) << "Done SRC" << std::setw(8)
               << " ";
            log<level::DEBUG>(fmt::format("{}", ss.str()).c_str());
            powerOpState = PowerOpState::buttonNotPressed;
            // emit pressedLong signal to shutdown the system
            sdbusplus::message::message msg; // dummy
            longPowerPressed(msg);
            powerOpTimer->setEnabled(false);
            return;
        }

        auto countDown = std::chrono::duration_cast<std::chrono::seconds>(
                             getPressTime() - now)
                             .count();

        // TODO: FPO SRC code and countdown to Op-Panel
        ss.clear();
        ss << std::right << std::setw(8) << "FPO SRC" << std::setw(8)
           << countDown;
        log<level::DEBUG>(fmt::format("{}", ss.str()).c_str());
    }
}

} // namespace button
} // namespace phosphor
