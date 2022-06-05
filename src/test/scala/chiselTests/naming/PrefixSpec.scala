// SPDX-License-Identifier: Apache-2.0

package chiselTests.naming

import chisel3._
import chisel3.stage.ChiselStage
import chisel3.aop.Select
import chisel3.experimental.{dump, noPrefix, prefix, treedump, withSuggestedName}
import chiselTests.{ChiselPropSpec, Utils}
import firrtl.annotations.ModuleName

class PrefixSpec extends ChiselPropSpec with Utils {
  implicit val minimumMajorVersion: Int = 12
  property("Scala plugin should interact with prefixing so last plugin name wins?") {
    class Test extends Module {
      def builder(): UInt = {
        val wire1 = Wire(UInt(3.W))
        val wire2 = Wire(UInt(3.W))
        wire2
      }

      {
        val x1 = prefix("first") {
          builder()
        }
      }
      {
        val x2 = prefix("second") {
          builder()
        }
      }
    }
    aspectTest(() => new Test) { top: Test =>
      Select.wires(top).map(_.instanceName) should be(List("x1_first_wire1", "x1", "x2_second_wire1", "x2"))
    }
  }

  property("Nested prefixes should work") {
    class Test extends Module {
      def builder2(): UInt = {
        val wire1 = Wire(UInt(3.W))
        val wire2 = Wire(UInt(3.W))
        wire2
      }
      def builder(): UInt = {
        val wire1 = Wire(UInt(3.W))
        val wire2 = Wire(UInt(3.W))
        prefix("foo") {
          builder2()
        }
      }
      { val x1 = builder() }
      { val x2 = builder() }
    }
    aspectTest(() => new Test) { top: Test =>
      Select.wires(top).map(_.instanceName) should be(
        List(
          "x1_wire1",
          "x1_wire2",
          "x1_foo_wire1",
          "x1",
          "x2_wire1",
          "x2_wire2",
          "x2_foo_wire1",
          "x2"
        )
      )
    }
  }

  property("Prefixing seeded with signal") {
    class Test extends Module {
      def builder(): UInt = {
        val wire = Wire(UInt(3.W))
        wire := 3.U
        wire
      }
      {
        val x1 = Wire(UInt(3.W))
        x1 := {
          builder()
        }
        val x2 = Wire(UInt(3.W))
        x2 := {
          builder()
        }
      }
    }
    aspectTest(() => new Test) { top: Test =>
      Select.wires(top).map(_.instanceName) should be(List("x1", "x1_wire", "x2", "x2_wire"))
    }
  }

  property("Automatic prefixing should work") {

    class Test extends Module {
      def builder(): UInt = {
        val a = Wire(UInt(3.W))
        val b = Wire(UInt(3.W))
        b
      }

      {
        val ADAM = builder()
        val JACOB = builder()
      }
    }
    aspectTest(() => new Test) { top: Test =>
      Select.wires(top).map(_.instanceName) should be(List("ADAM_a", "ADAM", "JACOB_a", "JACOB"))
    }
  }

  property("No prefixing annotation on defs should work") {

    class Test extends Module {
      def builder(): UInt = noPrefix {
        val a = Wire(UInt(3.W))
        val b = Wire(UInt(3.W))
        b
      }

      { val noprefix = builder() }
    }
    aspectTest(() => new Test) { top: Test =>
      Select.wires(top).map(_.instanceName) should be(List("a", "noprefix"))
    }
  }

  property("Prefixing on temps should work") {

    class Test extends Module {
      def builder(): UInt = {
        val a = Wire(UInt(3.W))
        val b = Wire(UInt(3.W))
        a +& (b * a)
      }

      { val blah = builder() }
    }
    aspectTest(() => new Test) { top: Test =>
      Select.ops(top).map(x => (x._1, x._2.instanceName)) should be(
        List(
          ("mul", "_blah_T"),
          ("add", "blah")
        )
      )
    }
  }

  property("Prefixing should not leak into child modules") {
    class Child extends Module {
      {
        val wire = Wire(UInt())
      }
    }

    class Test extends Module {
      {
        val child = prefix("InTest") {
          Module(new Child)
        }
      }
    }
    aspectTest(() => new Test) { top: Test =>
      Select.wires(Select.instances(top).head).map(_.instanceName) should be(List("wire"))
    }
  }

  property("Prefixing should not leak into child modules, example 2") {
    class Child extends Module {
      {
        val wire = Wire(UInt())
      }
    }

    class Test extends Module {
      val x = IO(Input(UInt(3.W)))
      val y = {
        lazy val module = new Child
        val child = Module(module)
      }
    }
    aspectTest(() => new Test) { top: Test =>
      Select.wires(Select.instances(top).head).map(_.instanceName) should be(List("wire"))
    }
  }

  property("Instance names should not be added to prefix") {
    class Child(tpe: UInt) extends Module {
      {
        val io = IO(Input(tpe))
      }
    }

    class Test extends Module {
      {
        lazy val module = {
          val x = UInt(3.W)
          new Child(x)
        }
        val child = Module(module)
      }
    }
    aspectTest(() => new Test) { top: Test =>
      Select.ios(Select.instances(top).head).map(_.instanceName) should be(List("clock", "reset", "io"))
    }
  }

  property("Prefixing should not be caused by nested Iterable[Iterable[Any]]") {
    class Test extends Module {
      {
        val iia = {
          val wire = Wire(UInt(3.W))
          List(List("Blah"))
        }
      }
    }
    aspectTest(() => new Test) { top: Test =>
      Select.wires(top).map(_.instanceName) should be(List("wire"))
    }
  }

  property("Prefixing should be caused by nested Iterable[Iterable[Data]]") {
    class Test extends Module {
      {
        val iia = {
          val wire = Wire(UInt(3.W))
          List(List(3.U))
        }
      }
    }
    aspectTest(() => new Test) { top: Test =>
      Select.wires(top).map(_.instanceName) should be(List("iia_wire"))
    }
  }

  property("Prefixing should be the prefix during the last call to autoName/suggestName") {
    class Test extends Module {
      {
        val wire = {
          val x = withSuggestedName("mywire")(Wire(UInt(3.W)))
          x
        }
      }
    }
    aspectTest(() => new Test) { top: Test =>
      Select.wires(top).map(_.instanceName) should be(List("mywire"))
      Select.wires(top).map(_.instanceName) shouldNot be(List("wire_mywire"))
    }
  }

  property("Prefixing have intuitive behavior") {
    class Test extends Module {
      {
        val wire = {
          val x = withSuggestedName("mywire") { Wire(UInt(3.W)) }
          val y = withSuggestedName("mywire2") { Wire(UInt(3.W)) }
          y := x
          y
        }
      }
    }
    aspectTest(() => new Test) { top: Test =>
      Select.wires(top).map(_.instanceName) should be(List("wire_mywire", "mywire2"))
    }
  }

  property("Prefixing on connection to subfields work") {
    class Test extends Module {
      {
        val wire = Wire(new Bundle {
          val x = UInt(3.W)
          val y = UInt(3.W)
          val vec = Vec(4, UInt(3.W))
        })
        wire.x := RegNext(3.U)
        wire.y := RegNext(3.U)
        wire.vec(0) := RegNext(3.U)
        wire.vec(wire.x) := RegNext(3.U)
        wire.vec(1.U) := RegNext(3.U)
      }
    }
    aspectTest(() => new Test) { top: Test =>
      Select.registers(top).map(_.instanceName) should be(
        List(
          "wire_x_REG",
          "wire_y_REG",
          "wire_vec_0_REG",
          "wire_vec_REG",
          "wire_vec_1_REG"
        )
      )
    }
  }

  property("Prefixing on connection to IOs should work") {
    class Child extends Module {
      val in = IO(Input(UInt(3.W)))
      val out = IO(Output(UInt(3.W)))
      out := RegNext(in)
    }
    class Test extends Module {
      {
        val child = Module(new Child)
        child.in := RegNext(3.U)
      }
    }
    aspectTest(() => new Test) { top: Test =>
      Select.registers(top).map(_.instanceName) should be(
        List(
          "child_in_REG"
        )
      )
      Select.registers(Select.instances(top).head).map(_.instanceName) should be(
        List(
          "out_REG"
        )
      )
    }
  }

  property("Prefixing on bulk connects should work") {
    class Child extends Module {
      val in = IO(Input(UInt(3.W)))
      val out = IO(Output(UInt(3.W)))
      out := RegNext(in)
    }
    class Test extends Module {
      {
        val child = Module(new Child)
        child.in <> RegNext(3.U)
      }
    }
    aspectTest(() => new Test) { top: Test =>
      Select.registers(top).map(_.instanceName) should be(
        List(
          "child_in_REG"
        )
      )
      Select.registers(Select.instances(top).head).map(_.instanceName) should be(
        List(
          "out_REG"
        )
      )
    }
  }

  property("Connections should use the non-prefixed name of the connected Data") {
    class Test extends Module {
      prefix("foo") {
        val x = Wire(UInt(8.W))
        x := {
          val w = Wire(UInt(8.W))
          w := 3.U
          w + 1.U
        }
      }
    }
    aspectTest(() => new Test) { top: Test =>
      Select.wires(top).map(_.instanceName) should be(List("foo_x", "foo_x_w"))
    }
  }

  property("Connections to aggregate fields should use the non-prefixed aggregate name") {
    class Test extends Module {
      prefix("foo") {
        val x = Wire(new Bundle { val bar = UInt(8.W) })
        x.bar := {
          val w = Wire(new Bundle { val fizz = UInt(8.W) })
          w.fizz := 3.U
          w.fizz + 1.U
        }
      }
    }
    aspectTest(() => new Test) { top: Test =>
      Select.wires(top).map(_.instanceName) should be(List("foo_x", "foo_x_bar_w"))
    }
  }

  property("Prefixing with wires in recursive functions should grow linearly") {
    class Test extends Module {
      def func(bools: Seq[Bool]): Bool = {
        if (bools.isEmpty) true.B
        else {
          val w = Wire(Bool())
          w := bools.head && func(bools.tail)
          w
        }
      }
      val in = IO(Input(Vec(4, Bool())))
      val x = func(in)
    }
    aspectTest(() => new Test) { top: Test =>
      Select.wires(top).map(_.instanceName) should be(List("x", "x_w_w", "x_w_w_w", "x_w_w_w_w"))
    }
  }

  property("Prefixing should work for verification ops") {
    class Test extends Module {
      val foo, bar = IO(Input(UInt(8.W)))

      {
        val x5 = {
          val x1 = chisel3.assert(1.U === 1.U)
          val x2 = cover(foo =/= bar)
          val x3 = chisel3.assume(foo =/= 123.U)
          val x4 = printf("foo = %d\n", foo)
          x1
        }
      }
    }
    val chirrtl = ChiselStage.emitChirrtl(new Test)
    (chirrtl should include).regex("assert.*: x5")
    (chirrtl should include).regex("cover.*: x5_x2")
    (chirrtl should include).regex("assume.*: x5_x3")
    (chirrtl should include).regex("printf.*: x5_x4")
  }

  property("withSuggestedName should work for a Wire") {
    class Test extends Module {
      def builder(): UInt = {
        val wire1 = Wire(UInt(3.W))
        val wire2 = Wire(UInt(3.W))
        wire2
      }

      {
        val x = withSuggestedName("second") {
          builder()
        }
      }
    }
    aspectTest(() => new Test) { top: Test =>
      Select.wires(top).map(_.instanceName) should be(List("x_wire1", "second"))
    }
  }

  property("withSuggestedName should work for an IO") {
    class Test extends RawModule {
      def builder(): UInt = {
        val wire1 = IO(Input(UInt(3.W)))
        val wire2 = IO(Output(UInt(3.W)))
        wire2 := wire1
        wire2
      }

      {
        val x = withSuggestedName("second") {
          builder()
        }
      }
    }
    aspectTest(() => new Test) { top: Test =>
      Select.ios(top).map(_.instanceName) should be(List("x_wire1", "second"))
    }
  }

  property("withSuggestedName should work for a Module") {
    class Test extends Module {
      def builder(): UInt = {
        val wire1 = IO(Input(UInt(3.W)))
        val wire2 = IO(Output(UInt(3.W)))
        wire2 := wire1
        wire2
      }

      {
        val x = withSuggestedName("submodule") {
          Module(new Module {
            builder()
          })
        }
      }
    }
    aspectTest(() => new Test) { top: Test =>
      Select.instances(top).map(_.instanceName) should be(List("submodule"))
    }
  }

  property("withSuggestedName should work for tuples") {
    class Test extends RawModule {
      def builder(): (UInt, UInt) = {
        val wire1 = IO(Input(UInt(3.W)))
        val wire2 = IO(Output(UInt(3.W)))
        wire2 := wire1
        (wire1, wire2)
      }

      {
        val x = withSuggestedName(Seq("first", "second")) {
          builder()
        }
      }
    }
    aspectTest(() => new Test) { top: Test =>
      Select.ios(top).map(_.instanceName) should be(List("first", "second"))
    }
  }

  property("withSuggestedName should error for tuples of things that aren't HasId") {
    class Test extends RawModule {
      def builder(): (UInt, UInt, Int) = {
        val wire1 = IO(Input(UInt(3.W)))
        val wire2 = IO(Output(UInt(3.W)))
        wire2 := wire1
        (wire1, wire2, 42)
      }

      {
        val x = withSuggestedName(Seq("first", "second", "forty-two")) {
          builder()
        }
      }
    }
    val caught = intercept[ClassCastException] {
      ChiselStage.elaborate(new Test)
    }
    caught.getMessage should include("cannot be cast to chisel3.internal.HasId")

  }

  property("withSuggestedName should error for something build outside of it") {
    class Test extends Module {
      def builder(): UInt = {
        val wire1 = Wire(UInt(3.W))
        val wire2 = Wire(UInt(3.W))
        wire2
      }

      {
        val x = builder()
        val y = withSuggestedName("bad") { x }
      }
    }
    val caught = intercept[IllegalArgumentException] { // Result type: IndexOutOfBoundsException
      ChiselStage.elaborate(new Test)
    }
    caught.getMessage should include("Cannot call withSuggestedName(bad){...} on already created hardware")

  }

}
