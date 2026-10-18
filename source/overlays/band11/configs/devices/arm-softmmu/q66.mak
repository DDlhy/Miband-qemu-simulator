CONFIG_XIAOMI_Q66=y
# Current ARM TCG also links A-profile GICv5 helpers; no GIC is instantiated
# by the q66 board, which uses the Cortex-M NVIC.
CONFIG_ARM_GICV5=y
