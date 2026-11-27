gen_tbl.py flattens the RTL8821A MAC / baseband / AGC / RF tables of the Realtek Linux driver
(hal/phydm/rtl8821a/halhwimg8821a_*.c, from morrownr/8821au-20210708) into rtl8821au\src\rtl8821_tbl.c.
The REF and OUT paths at the top are the author's; edit them, or pass them on the command line. Run it with Python 3.

  python3 gen_tbl.py <REF> <OUT>               USB tables (rtl8821au\src\rtl8821_tbl.c)
  python3 gen_tbl.py <REF> <OUT> pcie          PCIe tables (rtl8821ae\src\rtw\rtw_tbl.c)

The two differ only in some RF-A entries (the PA / LNA bias tables are conditional on the interface).
