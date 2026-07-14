/*
 * Copyright 2019 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

package com.google.googlesql;

import com.google.common.base.Preconditions;
import com.google.errorprone.annotations.Immutable;
import com.google.googlesql.GoogleSQLOptions.ProductMode;
import com.google.googlesql.GoogleSQLType.TypeKind;
import com.google.googlesql.GoogleSQLType.TypeProto;
import java.util.Objects;

/** Represents a DeclarativeType in the GoogleSQL type system. */
@Immutable
public final class DeclarativeType extends Type {

  @SuppressWarnings("Immutable")
  private final DeclarativeTypeDescriptor descriptor;

  DeclarativeType(DeclarativeTypeDescriptor descriptor) {
    super(TypeKind.TYPE_DECLARATIVE);
    this.descriptor = descriptor;
  }

  @Override
  public String typeName(ProductMode productMode) {
    return descriptor.getDisplayName();
  }

  @Override
  public String debugString(boolean details) {
    return descriptor.getDisplayName();
  }

  @Override
  public DeclarativeType asDeclarativeType() {
    return this;
  }

  public DeclarativeTypeDescriptor.TypeId getId() {
    return descriptor.getTypeId();
  }

  public Type getBackingType() {
    return descriptor.getBackingType();
  }

  public boolean isGoogleSqlBuiltin() {
    return descriptor.getTypeId().isGoogleSqlBuiltin();
  }

  public boolean isIdenticalTo(DeclarativeType other) {
    return descriptor.isIdenticalTo(other.descriptor);
  }

  @Override
  public void serialize(
      TypeProto.Builder typeProtoBuilder, FileDescriptorSetsBuilder fileDescriptorSetsBuilder) {
    typeProtoBuilder.setTypeKind(TypeKind.TYPE_DECLARATIVE);
    descriptor.serialize(typeProtoBuilder.getDeclarativeTypeBuilder(), fileDescriptorSetsBuilder);
  }

  @Override
  public int hashCode() {
    return Objects.hashCode(descriptor.getTypeId());
  }

  @SuppressWarnings("ReferenceEquality")
  public static boolean equalsImpl(
      DeclarativeType type1, DeclarativeType type2, boolean equivalent) {
    if (type1 == type2) {
      return true;
    }
    if (type1 == null || type2 == null) {
      return false;
    }
    // Identity is determined by TypeId. Identical components are checked.
    if (!Objects.equals(type1.descriptor.getTypeId(), type2.descriptor.getTypeId())) {
      return false;
    }

    Preconditions.checkArgument(
        type1.descriptor.isIdenticalTo(type2.descriptor), "Conflicting type definitions");

    return true;
  }
}
