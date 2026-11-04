"""A link lost mid-update: the device keeps its state for a while, and the host goes on."""

from simulator import START_RESUME, Device, Link, answer_to_activate, image

import dfukit


def test_resume_after_the_session_was_suspended() -> None:
    device = Device()
    ram = device.add_board()
    ram.behave(window=4096)
    data = image(100_000)
    link = Link(device, data)
    link.run_until(lambda: link.client.acked >= 50_000)

    link.lose_link()
    link.run(20_000)
    assert link.client.status == dfukit.Status.TIMEOUT
    link.run_until(lambda: device.state == dfukit.SessionState.SUSPENDED)
    assert device.taken >= 50_000

    link.resume()

    assert link.run().client.state == dfukit.ClientState.DONE
    assert ram.begins == 1, "not started over"
    assert ram.activated and ram.image == data


def test_resume_with_no_state_on_the_device_starts_over() -> None:
    device = Device()
    ram = device.add_board()
    data = image(30_000)

    link = Link(device, data, flags=START_RESUME).run()

    assert link.client.state == dfukit.ClientState.DONE
    assert ram.begins == 1 and ram.image == data


def test_new_address_takes_a_running_session_over() -> None:
    device = Device()
    ram = device.add_board()
    ram.behave(window=4096)
    data = image(60_000)
    link = Link(device, data)
    link.run_until(lambda: link.client.acked >= 20_000)

    link.new_address()
    link.resume()

    assert link.run().client.state == dfukit.ClientState.DONE
    assert ram.begins == 1 and ram.activated and ram.image == data


def test_new_address_while_finishing_does_not_send_the_image_again() -> None:
    device = Device()
    ram = device.add_board()
    ram.behave(window=4096, pending=1000)
    link = Link(device, image(30_000))
    link.run_until(lambda: link.client.state == dfukit.ClientState.FINISHING)

    link.new_address()
    ram.behave(pending=0)
    resent = link.client.resent
    link.resume()

    assert link.run().client.state == dfukit.ClientState.DONE
    assert ram.begins == 1 and link.client.resent == resent


def test_a_suspended_session_is_dropped_after_its_time() -> None:
    device = Device()
    ram = device.add_board()
    ram.behave(window=4096)
    link = Link(device, image(100_000))
    link.run_until(lambda: link.client.acked >= 50_000)

    link.lose_link()
    link.idle_time(11_000)
    assert device.state == dfukit.SessionState.SUSPENDED
    link.idle_time(61_000)

    assert device.state == dfukit.SessionState.IDLE and ram.aborts == 1


def test_resume_after_the_answer_to_activate_was_lost_does_not_start_over() -> None:
    device = Device()
    ram = device.add_board()
    data = image(20_000)
    link = Link(device, data)
    link.lose = answer_to_activate
    link.run_until(lambda: ram.activated)
    assert device.state == dfukit.SessionState.IDLE

    link.resume()

    assert link.run().client.state == dfukit.ClientState.DONE
    assert ram.begins == 1, "begin would erase the image just activated"
    assert ram.image == data


def test_resume_of_another_image_after_activate_starts_over() -> None:
    device = Device()
    ram = device.add_board()
    Link(device, image(20_000)).run()
    data = image(10_000, seed=2)

    assert Link(device, data, flags=START_RESUME).run().client.state == dfukit.ClientState.DONE
    assert ram.begins == 2 and ram.image == data
