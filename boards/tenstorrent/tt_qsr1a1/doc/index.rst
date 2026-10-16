.. _tt_qsr1a1:

TT QSR1A1 Board
###############

Overview
********

The ``tt_qsr1a1`` board is the zephyr board target for the QSR1.A1 package,
consisting of 1 Keraunos chiplet (the IO chiplet), 4 Mimir chiplets (the
memory chiplets) and 1 Quasar chiplet in the Grendel Family.

Hardware
********

The QSR1A1 board consists of:

- 4 Mimir chiplets (M0-M3), each with an SMC (``tt_mimir``)
- 1 Quasar chiplet (Q0) with an SMC (``tt_quasar``)
- 1 Keraunos chiplet (K) with an SMC (``tt_keraunos``)

Each SMC is a Grendel SMC and shares the Grendel SMC devicetree
(``tt_grendel_smc.dtsi``), which provides:

- RISC-V cores
- SRAM
- SiFive CLINT interrupt controller
- SiFive PLIC interrupt controller
- Cadence I3C controllers

Supported targets
*****************

.. list-table::
   :header-rows: 1

   * - Board target
     - Chiplet
   * - ``tt_qsr1a1/tt_keraunos/smc``
     - Keraunos
   * - ``tt_qsr1a1/tt_mimir/smc``
     - Mimir (all 4 instances)
   * - ``tt_qsr1a1/tt_quasar/smc``
     - Quasar

Interconnects
*************

- **I3C1 Connection**: I3C1 of each Mimir chiplet and of the Quasar chiplet is
  connected to I3C1 of the Keraunos chiplet. The Mimir and Quasar chiplets
  operate as I3C targets, while the Keraunos chiplet operates as the I3C
  controller.

  The Keraunos devicetree describes the remote chiplets as
  ``tenstorrent,smc-remoteproc`` devices on ``i3c1``, identified by their I3C
  PID:

  .. list-table::
     :header-rows: 1

     * - Chiplet
       - Node
       - PID
     * - M0
       - ``remoteproc_m0``
       - ``0a7c0013f000``
     * - M1
       - ``remoteproc_m1``
       - ``0a7c0013f001``
     * - M2
       - ``remoteproc_m2``
       - ``0a7c0013f002``
     * - M3
       - ``remoteproc_m3``
       - ``0a7c0013f003``
     * - Q0
       - ``remoteproc_q0``
       - ``0a7c0013f004``

Console
*******

Each SMC uses a virtual UART (``tenstorrent,vuart``) as its console and shell
UART. The descriptor address is published in ``SCRATCH_2`` so that host tools
such as ``keraunos_vuart_console`` can locate it.

System Clock
************

The CLINT timebase is 1MHz, yielding a rate of 1,000,000 ticks per second.
