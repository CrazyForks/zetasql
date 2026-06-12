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

package com.google.googlesql.resolvedast;

import static com.google.common.truth.Truth.assertThat;

import com.google.googlesql.GoogleSQLCollation.CollationProto;
import com.google.googlesql.GoogleSQLTypeModifiers.TypeModifiersProto;
import com.google.googlesql.GoogleSQLTypeParameters.StringTypeParametersProto;
import com.google.googlesql.TypeParameters;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.junit.runners.JUnit4;

@RunWith(JUnit4.class)

public class TypeModifiersTest {

  @Test
  public void testEmptyTypeModifiers() {
    TypeModifiers typeModifiers = new TypeModifiers();
    assertThat(typeModifiers.isEmpty()).isTrue();
    assertThat(typeModifiers.debugString()).isEqualTo("null");
  }

  @Test
  public void testTypeModifiersWithTypeParameters() {
    StringTypeParametersProto stringProto =
        StringTypeParametersProto.newBuilder().setMaxLength(10).build();
    TypeParameters typeParameters = new TypeParameters(stringProto);
    TypeModifiers typeModifiers =
        new TypeModifiers(typeParameters, CollationProto.getDefaultInstance());

    assertThat(typeModifiers.isEmpty()).isFalse();
    assertThat(typeModifiers.debugString()).isEqualTo("type_parameters:(max_length=10)");
  }

  @Test
  public void testTypeModifiersWithCollation() {
    CollationProto collationProto = CollationProto.newBuilder().setCollationName("und:ci").build();
    TypeModifiers typeModifiers = new TypeModifiers(new TypeParameters(), collationProto);

    assertThat(typeModifiers.isEmpty()).isFalse();
    assertThat(typeModifiers.debugString()).isEqualTo("collation:und:ci");
  }

  @Test
  public void testTypeModifiersWithTypeParametersAndCollation() {
    StringTypeParametersProto stringProto =
        StringTypeParametersProto.newBuilder().setMaxLength(10).build();
    TypeParameters typeParameters = new TypeParameters(stringProto);
    CollationProto collationProto = CollationProto.newBuilder().setCollationName("und:ci").build();
    TypeModifiers typeModifiers = new TypeModifiers(typeParameters, collationProto);

    assertThat(typeModifiers.isEmpty()).isFalse();
    assertThat(typeModifiers.debugString())
        .isEqualTo("type_parameters:(max_length=10), collation:und:ci");
  }

  @Test
  public void testSerializeDeserialize() {
    StringTypeParametersProto stringProto =
        StringTypeParametersProto.newBuilder().setMaxLength(10).build();
    TypeParameters typeParameters = new TypeParameters(stringProto);
    CollationProto collationProto = CollationProto.newBuilder().setCollationName("und:ci").build();
    TypeModifiers typeModifiers = new TypeModifiers(typeParameters, collationProto);

    TypeModifiersProto proto = typeModifiers.serialize();
    TypeModifiers deserialized = TypeModifiers.deserialize(proto);

    assertThat(deserialized.isEmpty()).isFalse();
    assertThat(deserialized.debugString()).isEqualTo(typeModifiers.debugString());
  }
}
