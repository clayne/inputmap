/*

Copyright 2017, Rodrigo Rivas Costa <rodrigorivascosta@gmail.com>

This file is part of inputmap.

inputmap is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

inputmap is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with inputmap.  If not, see <http://www.gnu.org/licenses/>.

*/

#include <linux/uinput.h>
#include <sys/epoll.h>
#include <algorithm>
#include "outputdev.h"
#include "inputdev.h"
#include "event-codes.h"
#include "devinput-parser.h"

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
    if (!bus.empty())
        us.id.bustype = bus_id(bus.c_str());
    else
        us.id.bustype = BUS_VIRTUAL;
    us.id.version = parse_int(version, 1);
    us.id.vendor = parse_hex_int(vendor, 0);
    us.id.product = parse_hex_int(product, 0);

    strcpy(us.name, name.c_str());

    m_fd = FD_open("/dev/uinput", O_RDWR);
    test(ioctl(m_fd.get(), UI_SET_PHYS, phys.c_str()), "UI_SET_PHYS");

    bool has_rel = false;
    for (const auto &kv : g_rel_names)
    {
        if (!kv.name)
            continue;
        std::string ref = ini.find_single_value(kv.name);
        if (ref.empty())
            continue;
        m_rel.emplace_back(kv.id, parse_ref(ref, inputFinder));
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
        m_key.emplace_back(kv.id, parse_ref(ref, inputFinder));
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
        m_abs.emplace_back(kv.id, parse_ref(ref, inputFinder));
        if (!has_abs)
        {
            test(ioctl(m_fd.get(), UI_SET_EVBIT, EV_ABS), "EV_ABS");
            has_abs = true;
        }
        uinput_abs_setup abs = {};
        abs.code = kv.id;
        abs.absinfo.minimum = -32767;
        abs.absinfo.maximum = 32767; //TODO: configure ABS range
        test(ioctl(m_fd.get(), UI_ABS_SETUP, &abs), "abs");
    }

    bool has_ff = false;
    for (const auto &kv : g_ff_names)
    {
        if (!kv.name)
            continue;
        std::string ref = ini.find_single_value(kv.name);
        if (ref.empty())
            continue;
        auto pref = parse_ref(ref, inputFinder);
        auto xref = dynamic_cast<ValueRef*>(pref.get());
        if (!xref || xref->get_value_id().type != EV_FF || xref->get_value_id().code != kv.id)
        {
            throw std::runtime_error("FF ref must be a simple reference to the same FF value");
        }
        pref.release();
        m_ff.emplace_back(kv.id, std::unique_ptr<ValueRef>(xref));
        if (!has_ff)
        {
            us.ff_effects_max = 16;
            test(ioctl(m_fd.get(), UI_SET_EVBIT, EV_FF), "EV_FF");
            has_ff = true;
        }
        test(ioctl(m_fd.get(), UI_SET_FFBIT, kv.id), "UI_SET_FFBIT");
    }

    test(ioctl(m_fd.get(), UI_DEV_SETUP, &us), "UI_DEV_SETUP");
    test(ioctl(m_fd.get(), UI_DEV_CREATE, 0), "UI_DEV_CREATE");
}

inline input_event create_event(int type, int code, int value)
{
    input_event ev;
    ev.type = type;
    ev.code = code;
    ev.value = value;
    return ev;
}

inline void do_event(std::vector<input_event> &evs, int type, int code, ValueExpr *ref)
{
    if (!ref)
        return;

    value_t value = ref->get_value();
    if (type == EV_ABS)
        value *= 32767;
    evs.push_back(create_event(type, code, static_cast<int>(value)));
}

void OutputDevice::sync()
{
    std::vector<input_event> evs;

    for (auto &v: m_rel)
        do_event(evs, EV_REL, v.first, v.second.get());
    for (auto &v: m_key)
        do_event(evs, EV_KEY, v.first, v.second.get());
    for (auto &v: m_abs)
        do_event(evs, EV_ABS, v.first, v.second.get());

    if (!evs.empty())
    {
        evs.push_back(create_event(EV_SYN, SYN_REPORT, 0));
        test(write(m_fd.get(), evs.data(), evs.size() * sizeof(input_event)), "write");
    }
}

ValueRef *OutputDevice::get_ff(int id)
{
    for (auto &v: m_ff)
        if (v.first == id)
            return v.second.get();
    return nullptr;
}

PollResult OutputDevice::on_poll(int event)
{
    if ((event & EPOLLIN) == 0)
        return PollResult::None;
    input_event ev;

    int res = read(fd(), &ev, sizeof(input_event));
    if (res == -1)
    {
        if (errno == EINTR)
            return PollResult::None;
        perror("output read");
        return PollResult::Error;
    }

    //printf("EV %d %d %d\n", ev.type, ev.code, ev.value);

    switch (ev.type)
    {
    case EV_UINPUT:
        switch (ev.code)
        {
        case UI_FF_UPLOAD:
            {
                uinput_ff_upload ff{};
                ff.request_id = ev.value;
                test(ioctl(m_fd.get(), UI_BEGIN_FF_UPLOAD, &ff), "UI_BEGIN_FF_UPLOAD");
                //printf("UPLOAD 0x%X, id=%d (%d, %d)\n", ff.effect.type, ff.effect.id, ff.effect.u.rumble.weak_magnitude, ff.effect.u.rumble.strong_magnitude);
                ValueRef *ffout = get_ff(ff.effect.type);
                auto device = ffout ? ffout->get_device() : nullptr;
                int out_id = ff.effect.id;
                int in_id = out_id < 0 || !device ? -EINVAL : device->ff_upload(ff.effect);
                ff.retval = in_id < 0 ? in_id : 0;
                test(ioctl(m_fd.get(), UI_END_FF_UPLOAD, &ff), "UI_END_FF_UPLOAD");
                if (in_id >= 0)
                {
                    if (static_cast<unsigned>(out_id) >= m_effects.size())
                        m_effects.resize(out_id + 1);
                    auto &effect = m_effects[out_id];
                    effect.device = device;
                    effect.input_id = in_id;
                }
            }
            break;
        case UI_FF_ERASE:
            {
                uinput_ff_erase ff{};
                ff.request_id = ev.value;
                test(ioctl(m_fd.get(), UI_BEGIN_FF_ERASE, &ff), "UI_BEGIN_FF_ERASE");
                //printf("ERASE %d\n", ff.effect_id);
                if (ff.effect_id < 0 || ff.effect_id >= m_effects.size())
                {
                    ff.retval = -EINVAL;
                }
                else
                {
                    auto &effect = m_effects[ff.effect_id];
                    auto device = effect.device.lock();
                    int in_id = effect.input_id;
                    effect = FFEffect{};
                    if (device)
                    {
                        int err = device->ff_erase(in_id);
                        if (err < 0)
                            ff.retval = err;
                        else
                            ff.retval = 0;
                    }
                    else
                    {
                        ff.retval = -EINVAL;
                    }
                }
                test(ioctl(m_fd.get(), UI_END_FF_ERASE, &ff), "UI_END_FF_ERASE");
            }
            break;
        }
        break;
    case EV_FF:
        {
            //printf("FF %s id=%d\n", ev.value? "start" : "stop", ev.code);
            if (ev.code >= 0 && ev.code < m_effects.size())
            {
                auto &effect = m_effects[ev.code];
                auto device = effect.device.lock();
                if (device)
                    device->ff_run(effect.input_id, ev.value != 0);
            }
        }
        break;
    }

    return PollResult::None;
}

