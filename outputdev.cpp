#include <linux/uinput.h>
#include <algorithm>
#include "outputdev.h"
#include "inputdev.h"
#include "event-codes.h"

OutputDevice::OutputDevice(const IniSection &ini, IInputByName &inputFinder)
{
    std::string name = ini.find_single_value("name");
    std::string phys = ini.find_single_value("phys");
    std::string bus = ini.find_single_value("bus");
    std::string vendor = ini.find_single_value("vendor");
    std::string product = ini.find_single_value("product");
    std::string version = ini.find_single_value("version");

    if (name.empty())
        name = "InputMap";
    uinput_setup us = {};
    if (bus == "USB")
        us.id.bustype = BUS_USB;
    else if (bus == "BLUETOOTH")
        us.id.bustype = BUS_BLUETOOTH;
    else if (bus == "PCI")
        us.id.bustype = BUS_PCI;
    else
        us.id.bustype = BUS_VIRTUAL;

    us.id.version = parse_int(version, 1);
    us.id.vendor = parse_hex_int(vendor, 0);
    us.id.product = parse_hex_int(product, 0);

    strcpy(us.name, name.c_str());

    m_fd = FD_open("/dev/uinput", O_RDWR|O_CLOEXEC);
    test(ioctl(m_fd.get(), UI_DEV_SETUP, &us), "UI_DEV_SETUP");
    test(ioctl(m_fd.get(), UI_SET_PHYS, phys.c_str()), "UI_SET_PHYS");

    //test(ioctl(m_fd.get(), UI_SET_EVBIT, EV_ABS), "EV_ABS");
    //test(ioctl(m_fd.get(), UI_SET_EVBIT, EV_KEY), "EV_KEY");

    /*
    uinput_abs_setup abs = {};
    abs.absinfo.minimum = 0;
    abs.absinfo.maximum = 0xFF;
    abs.absinfo.value = 0x7F;
    abs.code = ABS_X;
    test(ioctl(m_fd.get(), UI_ABS_SETUP, &abs), "ABS_X");
    abs.code = ABS_Y;
    test(ioctl(m_fd.get(), UI_ABS_SETUP, &abs), "ABS_Y");

    test(ioctl(m_fd.get(), UI_SET_KEYBIT, BTN_TRIGGER), "BTN_TRIGGER");
    */

    bool has_rel = false;
    for (const auto &kv : g_rel_names)
    {
        if (!kv.name)
            continue;
        std::string ref = ini.find_single_value(kv.name);
        if (ref.empty())
            continue;
        m_rel[kv.id] = parse_ref(ref, inputFinder);
        if (!has_rel)
        {
            test(ioctl(m_fd.get(), UI_SET_EVBIT, EV_REL), "EV_REL");
            has_rel = true;
        }
        test(ioctl(m_fd.get(), UI_SET_RELBIT, kv.id), "UI_SET_RELBIT");
    }

    bool has_key = false;
    for (const auto &kv : g_key_names)
    {
        if (!kv.name)
            continue;
        std::string ref = ini.find_single_value(kv.name);
        if (ref.empty())
            continue;
        m_key[kv.id] = parse_ref(ref, inputFinder);
        if (!has_key)
        {
            test(ioctl(m_fd.get(), UI_SET_EVBIT, EV_KEY), "EV_KEY");
            has_key = true;
        }
        test(ioctl(m_fd.get(), UI_SET_KEYBIT, kv.id), "UI_SET_KEYBIT");
    }

    bool has_abs = false;
    for (const auto &kv : g_abs_names)
    {
        if (!kv.name)
            continue;
        std::string ref = ini.find_single_value(kv.name);
        if (ref.empty())
            continue;
        m_abs[kv.id] = parse_ref(ref, inputFinder);
        if (!has_abs)
        {
            test(ioctl(m_fd.get(), UI_SET_EVBIT, EV_ABS), "EV_ABS");
            has_abs = true;
        }
        //test(ioctl(m_fd.get(), UI_SET_ABSBIT, kv.id), "UI_SET_ABSBIT");
        auto dev = m_abs[kv.id].device.lock();
        uinput_abs_setup abs = {};
        abs.code = kv.id;
        abs.absinfo = dev->get_absinfo(m_abs[kv.id].value_id.code);
        test(ioctl(m_fd.get(), UI_ABS_SETUP, &abs), "abs");
    }

    test(ioctl(m_fd.get(), UI_DEV_CREATE, 0), "UI_DEV_CREATE");
}

std::shared_ptr<InputDevice> OutputDevice::find_input(const std::string &name, IInputByName *inputFinder)
{
    auto it = std::find_if(m_inputs.begin(), m_inputs.end(), [&name](std::weak_ptr<InputDevice> &x)
            {
                auto p = x.lock();
                return p && p->name() == name;
            });
    if (it != m_inputs.end())
        return it->lock();

    std::shared_ptr<InputDevice> res;
    if (inputFinder)
    {
        res = inputFinder->find_input(name);
        if (res)
            m_inputs.push_back(res);
    }
    return res;
}

ValueRef OutputDevice::parse_ref(const std::string &desc, IInputByName &inputFinder)
{
    size_t dot = desc.find(".");
    if (dot == std::string::npos)
        throw std::runtime_error("invalid ref: " + desc);
    std::string sdev = desc.substr(0, dot);
    std::string saxis = desc.substr(dot + 1);
    auto dev = find_input(sdev, &inputFinder);
    if (!dev)
        throw std::runtime_error("unknown device in ref: " + desc);
    auto value_id = dev->parse_value(saxis);
    return ValueRef{dev, value_id};
}

inline input_event create_event(int type, int code, int value)
{
    input_event ev;
    ev.type = type;
    ev.code = code;
    ev.value = value;
    return ev;
}

inline void do_event(std::vector<input_event> &evs, int type, int code, ValueRef &ref)
{
    if (ref)
        evs.push_back(create_event(type, code, ref.get_value()));
}

void OutputDevice::sync()
{
    std::vector<input_event> evs;

    for (int i = 0; i < REL_CNT; ++i)
        do_event(evs, EV_REL, i, m_rel[i]);
    for (int i = 0; i < KEY_CNT; ++i)
        do_event(evs, EV_KEY, i, m_key[i]);
    for (int i = 0; i < ABS_CNT; ++i)
        do_event(evs, EV_ABS, i, m_abs[i]);

    if (!evs.empty())
    {
        evs.push_back(create_event(EV_SYN, SYN_REPORT, 0));
        test(write(m_fd.get(), evs.data(), evs.size() * sizeof(input_event)), "write");
    }
}
