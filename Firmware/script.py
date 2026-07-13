from machine import Pin, PWM, time_pulse_us
from time import sleep_us, sleep_ms


STBY = Pin(28, Pin.OUT)
STBY.value(1)


LEFT_IN1 = PWM(Pin(26)); LEFT_IN1.freq(5000)
LEFT_IN2 = PWM(Pin(27)); LEFT_IN2.freq(5000)


RIGHT_IN1 = PWM(Pin(29)); RIGHT_IN1.freq(5000)
RIGHT_IN2 = PWM(Pin(6));  RIGHT_IN2.freq(5000)


TRIG = Pin(4, Pin.OUT)
ECHO = Pin(3, Pin.IN)


MAX_SPEED = 50000
MIN_SPEED = 25000
SAFE_DIST = 60.0
TURN_DIST = 20.0


LEFT_TRIM = 1.0
RIGHT_TRIM = 1.0




def get_distance():
    TRIG.value(0)
    sleep_us(2)
    TRIG.value(1)
    sleep_us(10)
    TRIG.value(0)

    duration = time_pulse_us(ECHO, 1, 30000)
    if duration > 0:
        return (duration * 0.0343) / 2
    else:
        return -1

def drive(left_speed, right_speed):
    left_speed = int(left_speed * LEFT_TRIM)
    right_speed = int(right_speed * RIGHT_TRIM)

    left_speed = max(-65535, min(left_speed, 65535))
    right_speed = max(-65535, min(right_speed, 65535))


    if left_speed > 0:
        LEFT_IN1.duty_u16(left_speed)
        LEFT_IN2.duty_u16(0)
    elif left_speed < 0:
        LEFT_IN1.duty_u16(0)
        LEFT_IN2.duty_u16(abs(left_speed))
    else:
        LEFT_IN1.duty_u16(0); LEFT_IN2.duty_u16(0)


    if right_speed > 0:
        RIGHT_IN1.duty_u16(right_speed)
        RIGHT_IN2.duty_u16(0)
    elif right_speed < 0:
        RIGHT_IN1.duty_u16(0)
        RIGHT_IN2.duty_u16(abs(right_speed))
    else:
        RIGHT_IN1.duty_u16(0); RIGHT_IN2.duty_u16(0)

print("Booting up... Autonomous mode active!")

while True:
    dist = get_distance()


    if dist == -1 or dist > SAFE_DIST:
        drive(MAX_SPEED, MAX_SPEED)


    elif dist <= SAFE_DIST and dist > TURN_DIST:
        factor = (dist - TURN_DIST) / (SAFE_DIST - TURN_DIST)

        current_speed = int(MIN_SPEED + (factor * (MAX_SPEED - MIN_SPEED)))

        left_speed = current_speed
        right_speed = int(current_speed * factor)

        drive(left_speed, right_speed)

    elif dist <= TURN_DIST:
        drive(MAX_SPEED, -MAX_SPEED)
        sleep_ms(250)

    sleep_ms(50)
