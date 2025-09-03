# Open sensor

10-28v input.

esp32 (unspecific yet) connected to a gy-87 (MPU6050 HMC5883L BMP180) covering
 - Acceleration
 - gyro
 - magnetometer
 - pressure
 - temperature

And a quectel L80 GPS - i've had alot really good success with these, they are a supprisingly capable GPS that is heavily underrated. Mostly been used for timing in my world, but against a ublox 6 or MTK3339, the L80 is definitely supperiour is terms of stability. ublox is more configurable, but the only real parameter that ever really matter was delay propogation for the pps line and that has no purpose here (though the l80 does support it). The ublox has more configuartion around the sentences that it can send, but the L80 has enough configurability in its sentences to be good.
