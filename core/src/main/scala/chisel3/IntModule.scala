// SPDX-License-Identifier: Apache-2.0

package chisel3

import chisel3.experimental.{BaseModule, Param}
import chisel3.internal.BaseIntModule
import chisel3.internal.Builder.pushCommand
import chisel3.internal.firrtl._
import scala.annotation.nowarn

package internal {

  private[chisel3] abstract class BaseIntModule(intrinsicName : String) extends BaseModule {
    val intrinsic = intrinsicName
  }

}

package experimental {

  @nowarn("msg=class Port") // delete when Port becomes private
  abstract class IntModule(intrinsicName : String, val params: Map[String, Param] = Map.empty[String, Param]) extends BaseIntModule(intrinsicName) {
    private[chisel3] override def generateComponent(): Option[Component] = {
      require(!_closed, "Can't generate intmodule more than once")
      _closed = true

      // Ports are named in the same way as regular Modules
      namePorts()

      val firrtlPorts = getModulePorts.map {
        case port => Port(port, port.specifiedDirection, UnlocatableSourceInfo)
      }
      val component = DefIntModule(this, name, firrtlPorts, SpecifiedDirection.Unspecified, params)
      _component = Some(component)
      _component
    }

    private[chisel3] def initializeInParent(): Unit = {
      implicit val sourceInfo = UnlocatableSourceInfo
    }
  }
}
