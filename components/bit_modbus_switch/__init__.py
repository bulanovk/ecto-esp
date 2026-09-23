"""Bit-level switches on one shared holding register.

A copy of modbus_controller's ModbusSwitch whose write path is a mutex-guarded
read-modify-write: several switches on one register no longer clobber each
other's bits. The per-address registry (mutex + last polled value) lives in
bit_modbus_switch.h/.cpp; the platform schema lives in switch.py.
"""
