#!/usr/bin/python3

import unittest
import sys

sys.path.append('../util')
import iwd
from iwd import IWD
from iwd import PSKAgent
from iwd import NetworkType

from hostapd import HostapdCLI
from hostapd import hostapd_map

from wiphy import wiphy_map
from hwsim import Hwsim

import time

class Test(unittest.TestCase):

    def test_connection_success(self):
        hwsim = Hwsim()

        bss_radio = [None, None, None]
        bss_hostapd = [None, None, None]

        for wname in wiphy_map:
            wiphy = wiphy_map[wname]
            intf = list(wiphy.values())[0]

            if intf.config and '1' in intf.config:
                bss_idx = 0
            elif intf.config and '2' in intf.config:
                bss_idx = 1
            elif intf.config and '3' in intf.config:
                bss_idx = 2
            else:
                continue

            for path in hwsim.radios:
                radio = hwsim.radios[path]
                if radio.name == wname:
                    break

            bss_radio[bss_idx] = radio
            bss_hostapd[bss_idx] = HostapdCLI(intf)

        rule0 = hwsim.rules.create()
        rule0.source = bss_radio[0].addresses[0]
        rule0.bidirectional = True
        rule0.signal = -2000

        rule1 = hwsim.rules.create()
        rule1.source = bss_radio[1].addresses[0]
        rule1.bidirectional = True
        rule1.signal = -8000

        rule2 = hwsim.rules.create()
        rule2.source = bss_radio[2].addresses[0]
        rule2.bidirectional = True
        rule2.signal = -10000

        wd = IWD(True, '/tmp')

        psk_agent = PSKAgent("wrong_password")
        wd.register_psk_agent(psk_agent)

        devices = wd.list_devices(1)
        device = devices[0]

        condition = 'not obj.scanning'
        wd.wait_for_object_condition(device, condition)

        device.scan()

        condition = 'not obj.scanning'
        wd.wait_for_object_condition(device, condition)

        ordered_network = device.get_ordered_network("TestBlacklist")

        self.assertEqual(ordered_network.type, NetworkType.psk)

        condition = 'not obj.connected'
        wd.wait_for_object_condition(ordered_network.network_object, condition)

        with self.assertRaises(iwd.FailedEx):
            ordered_network.network_object.connect()

        wd.unregister_psk_agent(psk_agent)

        psk_agent = PSKAgent("secret123")
        wd.register_psk_agent(psk_agent)

        ordered_network.network_object.connect()

        # We failed to connect bss_hostapd[0], but with a bad password. Verify
        # that this did not trigger a blacklist and that we did reconnect
        # successfully to bss_hostapd[0]
        self.assertIn(device.address, bss_hostapd[0].list_sta())

        condition = 'obj.connected'
        wd.wait_for_object_condition(ordered_network.network_object, condition)

        del wd

    @classmethod
    def setUpClass(cls):
        pass

    @classmethod
    def tearDownClass(cls):
        IWD.clear_storage()

if __name__ == '__main__':
    unittest.main(exit=True)